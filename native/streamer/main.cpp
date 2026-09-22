#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sddl.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "FrameRing.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

extern "C" __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kDefaultWidth = 1920;
constexpr uint32_t kDefaultHeight = 1200;
constexpr uint32_t kDefaultFps = 60;
constexpr uint32_t kDefaultBitrate = 12'000'000;
constexpr uint16_t kDefaultPort = 5000;
constexpr size_t kMaxHandshakeBytes = 64 * 1024;
constexpr int kClientSendBufferBytes = 32 * 1024;
constexpr int kClientSendTimeoutMs = 2000;
constexpr uint32_t kPacketMagic = 0x31565353; // bytes: 53 53 56 31 ("SSV1")

std::atomic_bool g_stop{false};
std::atomic<SOCKET> g_listener{INVALID_SOCKET};
std::atomic<SOCKET> g_client{INVALID_SOCKET};

std::string HResultText(HRESULT hr) {
    char buffer[512]{};
    DWORD length = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buffer,
        static_cast<DWORD>(sizeof(buffer)),
        nullptr);
    std::string text = length == 0 ? "unknown error" : std::string(buffer, length);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

[[noreturn]] void ThrowHr(HRESULT hr, const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed (0x" + [&] {
        char hex[16]{};
        std::snprintf(hex, sizeof(hex), "%08lX", static_cast<unsigned long>(hr));
        return std::string(hex);
    }() + "): " + HResultText(hr));
}

void CheckHr(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        ThrowHr(hr, operation);
    }
}


std::string WsaText(int error) {
    char buffer[256]{};
    DWORD length = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(error),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buffer,
        static_cast<DWORD>(sizeof(buffer)),
        nullptr);
    std::string text = length == 0 ? "unknown socket error" : std::string(buffer, length);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}
bool EnablePrivilege(const wchar_t* privilegeName) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    LUID privilegeLuid{};
    bool enabled = LookupPrivilegeValueW(nullptr, privilegeName, &privilegeLuid) != FALSE;
    if (enabled) {
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = privilegeLuid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        enabled = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) != FALSE;
        if (enabled) {
            enabled = GetLastError() != ERROR_NOT_ALL_ASSIGNED;
        }
    }
    CloseHandle(token);
    return enabled;
}

[[noreturn]] void ThrowWin32(DWORD error, const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(error) + "): " + WsaText(static_cast<int>(error)));
}

bool IsValidUtf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
        if (first <= 0x7f) {
            ++i;
            continue;
        }
        size_t continuationCount = 0;
        unsigned char secondMin = 0x80;
        unsigned char secondMax = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) {
            continuationCount = 1;
        } else if (first == 0xe0) {
            continuationCount = 2;
            secondMin = 0xa0;
        } else if (first >= 0xe1 && first <= 0xec) {
            continuationCount = 2;
        } else if (first == 0xed) {
            continuationCount = 2;
            secondMax = 0x9f;
        } else if (first == 0xee || first == 0xef) {
            continuationCount = 2;
        } else if (first == 0xf0) {
            continuationCount = 3;
            secondMin = 0x90;
        } else if (first >= 0xf1 && first <= 0xf3) {
            continuationCount = 3;
        } else if (first == 0xf4) {
            continuationCount = 3;
            secondMax = 0x8f;
        } else {
            return false;
        }
        if (i + continuationCount >= text.size()) {
            return false;
        }
        const unsigned char second = static_cast<unsigned char>(text[i + 1]);
        if (second < secondMin || second > secondMax) {
            return false;
        }
        for (size_t j = 2; j <= continuationCount; ++j) {
            const unsigned char byte = static_cast<unsigned char>(text[i + j]);
            if (byte < 0x80 || byte > 0xbf) {
                return false;
            }
        }
        i += continuationCount + 1;
    }
    return true;
}

void AppendUtf8(std::string& output, uint32_t codePoint) {
    if (codePoint <= 0x7f) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else if (codePoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : current_(input.data()), end_(input.data() + input.size()) {}

    struct Handshake {
        int64_t protocol = 0;
        std::string device;
        int64_t width = 0;
        int64_t height = 0;
        std::vector<std::string> decoders;
        bool hasProtocol = false;
        bool hasDevice = false;
        bool hasWidth = false;
        bool hasHeight = false;
        bool hasDecoders = false;
    };

    bool ParseHandshake(Handshake& handshake, std::string& error) {
        SkipWhitespace();
        if (!Consume('{')) {
            error = "handshake must be a JSON object";
            return false;
        }
        SkipWhitespace();
        if (Consume('}')) {
            error = "handshake object is missing required fields";
            return false;
        }
        while (true) {
            std::string key;
            if (!ParseString(key)) {
                error = "handshake contains an invalid object key";
                return false;
            }
            SkipWhitespace();
            if (!Consume(':')) {
                error = "handshake object key is missing ':'";
                return false;
            }
            SkipWhitespace();
            bool ok = true;
            if (key == "protocol") {
                if (handshake.hasProtocol || !ParseInteger(handshake.protocol)) {
                    error = "protocol must be a unique integer";
                    return false;
                }
                handshake.hasProtocol = true;
            } else if (key == "device") {
                if (handshake.hasDevice || !ParseString(handshake.device)) {
                    error = "device must be a unique string";
                    return false;
                }
                handshake.hasDevice = true;
            } else if (key == "width") {
                if (handshake.hasWidth || !ParseInteger(handshake.width)) {
                    error = "width must be a unique integer";
                    return false;
                }
                handshake.hasWidth = true;
            } else if (key == "height") {
                if (handshake.hasHeight || !ParseInteger(handshake.height)) {
                    error = "height must be a unique integer";
                    return false;
                }
                handshake.hasHeight = true;
            } else if (key == "decoder") {
                if (handshake.hasDecoders || !ParseStringArray(handshake.decoders)) {
                    error = "decoder must be a unique string array";
                    return false;
                }
                handshake.hasDecoders = true;
            } else {
                ok = SkipValue();
            }
            if (!ok) {
                error = "handshake contains an invalid JSON value";
                return false;
            }
            SkipWhitespace();
            if (Consume('}')) {
                break;
            }
            if (!Consume(',')) {
                error = "handshake object is missing ','";
                return false;
            }
            SkipWhitespace();
        }
        SkipWhitespace();
        if (current_ != end_) {
            error = "handshake has trailing data after JSON object";
            return false;
        }
        if (!handshake.hasProtocol || handshake.protocol != 1) {
            error = "unsupported protocol; expected protocol=1";
            return false;
        }
        if (!handshake.hasDevice || handshake.device.empty()) {
            error = "device must be non-empty";
            return false;
        }
        if (!handshake.hasWidth || handshake.width <= 0 || handshake.width > 16384) {
            error = "width must be between 1 and 16384";
            return false;
        }
        if (!handshake.hasHeight || handshake.height <= 0 || handshake.height > 16384) {
            error = "height must be between 1 and 16384";
            return false;
        }
        if (!handshake.hasDecoders || handshake.decoders.size() != 1 || handshake.decoders[0] != "h264") {
            error = "decoder must be [\"h264\"]";
            return false;
        }
        return true;
    }

private:
    const char* current_;
    const char* end_;

    void SkipWhitespace() {
        while (current_ != end_ && (*current_ == ' ' || *current_ == '\t' || *current_ == '\r' || *current_ == '\n')) {
            ++current_;
        }
    }

    bool Consume(char expected) {
        if (current_ == end_ || *current_ != expected) {
            return false;
        }
        ++current_;
        return true;
    }

    bool ParseHex4(uint32_t& value) {
        if (end_ - current_ < 4) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *current_++;
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool ParseString(std::string& output) {
        if (!Consume('"')) {
            return false;
        }
        output.clear();
        while (current_ != end_) {
            const unsigned char c = static_cast<unsigned char>(*current_++);
            if (c == '"') {
                return true;
            }
            if (c < 0x20) {
                return false;
            }
            if (c != '\\') {
                output.push_back(static_cast<char>(c));
                continue;
            }
            if (current_ == end_) {
                return false;
            }
            const char escape = *current_++;
            switch (escape) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                uint32_t codePoint = 0;
                if (!ParseHex4(codePoint)) {
                    return false;
                }
                if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
                    if (end_ - current_ < 6 || current_[0] != '\\' || current_[1] != 'u') {
                        return false;
                    }
                    current_ += 2;
                    uint32_t low = 0;
                    if (!ParseHex4(low) || low < 0xdc00 || low > 0xdfff) {
                        return false;
                    }
                    codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
                    return false;
                }
                AppendUtf8(output, codePoint);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool ParseInteger(int64_t& value) {
        const char* start = current_;
        if (current_ != end_ && *current_ == '-') {
            ++current_;
        }
        if (current_ == end_ || *current_ < '0' || *current_ > '9') {
            current_ = start;
            return false;
        }
        if (*current_ == '0') {
            ++current_;
            if (current_ != end_ && *current_ >= '0' && *current_ <= '9') {
                current_ = start;
                return false;
            }
        } else {
            while (current_ != end_ && *current_ >= '0' && *current_ <= '9') {
                ++current_;
            }
        }
        if (current_ != end_ && (*current_ == '.' || *current_ == 'e' || *current_ == 'E')) {
            current_ = start;
            return false;
        }
        const auto result = std::from_chars(start, current_, value);
        if (result.ec != std::errc{} || result.ptr != current_) {
            current_ = start;
            return false;
        }
        return true;
    }

    bool ParseStringArray(std::vector<std::string>& output) {
        if (!Consume('[')) {
            return false;
        }
        SkipWhitespace();
        output.clear();
        if (Consume(']')) {
            return true;
        }
        while (true) {
            std::string value;
            if (!ParseString(value)) {
                return false;
            }
            output.push_back(std::move(value));
            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                return false;
            }
            SkipWhitespace();
        }
    }

    bool ParseLiteral(std::string_view literal) {
        if (end_ - current_ < static_cast<ptrdiff_t>(literal.size()) ||
            std::string_view(current_, literal.size()) != literal) {
            return false;
        }
        current_ += literal.size();
        return true;
    }

    bool SkipNumber() {
        const char* start = current_;
        if (current_ != end_ && *current_ == '-') {
            ++current_;
        }
        if (current_ == end_ || *current_ < '0' || *current_ > '9') {
            current_ = start;
            return false;
        }
        if (*current_ == '0') {
            ++current_;
        } else {
            while (current_ != end_ && *current_ >= '0' && *current_ <= '9') {
                ++current_;
            }
        }
        if (current_ != end_ && *current_ == '.') {
            ++current_;
            if (current_ == end_ || *current_ < '0' || *current_ > '9') {
                current_ = start;
                return false;
            }
            while (current_ != end_ && *current_ >= '0' && *current_ <= '9') {
                ++current_;
            }
        }
        if (current_ != end_ && (*current_ == 'e' || *current_ == 'E')) {
            ++current_;
            if (current_ != end_ && (*current_ == '+' || *current_ == '-')) {
                ++current_;
            }
            if (current_ == end_ || *current_ < '0' || *current_ > '9') {
                current_ = start;
                return false;
            }
            while (current_ != end_ && *current_ >= '0' && *current_ <= '9') {
                ++current_;
            }
        }
        return true;
    }

    bool SkipValue() {
        SkipWhitespace();
        if (current_ == end_) {
            return false;
        }
        if (*current_ == '"') {
            std::string ignored;
            return ParseString(ignored);
        }
        if (*current_ == '{') {
            ++current_;
            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            while (true) {
                std::string ignored;
                if (!ParseString(ignored)) {
                    return false;
                }
                SkipWhitespace();
                if (!Consume(':') || !SkipValue()) {
                    return false;
                }
                SkipWhitespace();
                if (Consume('}')) {
                    return true;
                }
                if (!Consume(',')) {
                    return false;
                }
                SkipWhitespace();
            }
        }
        if (*current_ == '[') {
            ++current_;
            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            while (true) {
                if (!SkipValue()) {
                    return false;
                }
                SkipWhitespace();
                if (Consume(']')) {
                    return true;
                }
                if (!Consume(',')) {
                    return false;
                }
                SkipWhitespace();
            }
        }
        if (*current_ == 't') return ParseLiteral("true");
        if (*current_ == 'f') return ParseLiteral("false");
        if (*current_ == 'n') return ParseLiteral("null");
        return SkipNumber();
    }
};

bool ReadHandshake(SOCKET socket, std::string& line, std::string& error) {
    line.clear();
    std::array<char, 2048> buffer{};
    while (line.size() < kMaxHandshakeBytes) {
        const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received == 0) {
            error = "client closed before sending handshake";
            return false;
        }
        if (received == SOCKET_ERROR) {
            const int code = WSAGetLastError();
            error = "recv(handshake) failed (" + std::to_string(code) + "): " + WsaText(code);
            return false;
        }
        for (int i = 0; i < received; ++i) {
            if (buffer[static_cast<size_t>(i)] == '\n') {
                return true;
            }
            line.push_back(buffer[static_cast<size_t>(i)]);
            if (line.size() == kMaxHandshakeBytes) {
                break;
            }
        }
    }
    error = "handshake exceeds 64 KiB or has no newline terminator";
    return false;
}

class PacketWriter {
public:
    PacketWriter() = default;
    explicit PacketWriter(SOCKET socket) : socket_(socket) {}

    bool SendAccessUnit(uint64_t frameId, uint64_t timestampUs, const uint8_t* payload, size_t payloadLength) {
        if (payloadLength > std::numeric_limits<uint32_t>::max()) {
            error_ = "encoded access unit exceeds uint32 packet length";
            return false;
        }
        std::array<uint8_t, 24> header{};
        WriteLe32(header.data(), kPacketMagic);
        WriteLe32(header.data() + 4, static_cast<uint32_t>(payloadLength));
        WriteLe64(header.data() + 8, frameId);
        WriteLe64(header.data() + 16, timestampUs);
        return SendBytes(header.data(), header.size()) && SendBytes(payload, payloadLength);
    }

    bool SendAnnexB(uint64_t frameId, uint64_t timestampUs, const uint8_t* payload, size_t payloadLength) {
        if (HasAnnexBStartCode(payload, payloadLength)) {
            return SendAccessUnit(frameId, timestampUs, payload, payloadLength);
        }
        std::vector<uint8_t> converted;
        if (!ConvertLengthPrefixed(payload, payloadLength, converted)) {
            error_ = "encoder returned an unrecognized H.264 access unit (" +
                std::to_string(payloadLength) + " bytes, prefix";
            for (std::size_t index = 0; index < std::min<std::size_t>(payloadLength, 16); ++index) {
                char hex[4]{};
                std::snprintf(hex, sizeof(hex), " %02X", payload[index]);
                error_ += hex;
            }
            error_ += ')';
            return false;
        }
        return SendAccessUnit(frameId, timestampUs, converted.data(), converted.size());
    }

    const std::string& error() const { return error_; }

private:
    SOCKET socket_ = INVALID_SOCKET;
    std::string error_;

    static void WriteLe32(uint8_t* destination, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) {
            destination[i] = static_cast<uint8_t>(value >> (i * 8));
        }
    }

    static void WriteLe64(uint8_t* destination, uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) {
            destination[i] = static_cast<uint8_t>(value >> (i * 8));
        }
    }

    static bool HasAnnexBStartCode(const uint8_t* payload, size_t length) {
        return length >= 3 && payload[0] == 0 && payload[1] == 0 &&
            ((payload[2] == 1) || (length >= 4 && payload[2] == 0 && payload[3] == 1));
    }

    static bool ConvertLengthPrefixed(const uint8_t* payload, size_t length, std::vector<uint8_t>& output) {
        auto readLength = [&](size_t offset, size_t bytes, uint32_t& value) {
            if (offset + bytes > length) {
                return false;
            }
            value = 0;
            for (size_t i = 0; i < bytes; ++i) {
                value = (value << 8) | payload[offset + i];
            }
            return true;
        };
        for (const size_t lengthBytes : {size_t(4), size_t(2)}) {
            size_t offset = 0;
            output.clear();
            bool valid = true;
            while (offset < length) {
                uint32_t nalLength = 0;
                if (!readLength(offset, lengthBytes, nalLength)) {
                    valid = false;
                    break;
                }
                offset += lengthBytes;
                if (nalLength == 0 || nalLength > length - offset) {
                    valid = false;
                    break;
                }
                output.insert(output.end(), {0, 0, 0, 1});
                output.insert(output.end(), payload + offset, payload + offset + nalLength);
                offset += nalLength;
            }
            if (valid && offset == length && !output.empty()) {
                return true;
            }
        }
        output.clear();
        return false;
    }

    bool SendBytes(const uint8_t* data, size_t length) {
        if (socket_ == INVALID_SOCKET) {
            return true;
        }
        while (length != 0) {
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(socket_, &writeSet);
            timeval timeout{};
            timeout.tv_sec = kClientSendTimeoutMs / 1000;
            timeout.tv_usec = (kClientSendTimeoutMs % 1000) * 1000;
            const int selected = select(0, nullptr, &writeSet, nullptr, &timeout);
            if (selected == SOCKET_ERROR) {
                const int code = WSAGetLastError();
                error_ = "select(send) failed (" + std::to_string(code) + "): " + WsaText(code);
                return false;
            }
            if (selected == 0) {
                error_ = "send timed out after " + std::to_string(kClientSendTimeoutMs) + " ms; client stopped reading";
                return false;
            }
            const int chunk = static_cast<int>(std::min<size_t>(length, static_cast<size_t>(std::numeric_limits<int>::max())));
            const int sent = send(socket_, reinterpret_cast<const char*>(data), chunk, 0);
            if (sent == SOCKET_ERROR) {
                const int code = WSAGetLastError();
                error_ = "send failed (" + std::to_string(code) + "): " + WsaText(code);
                return false;
            }
            if (sent == 0) {
                error_ = "send returned zero bytes; client disconnected";
                return false;
            }
            data += sent;
            length -= static_cast<size_t>(sent);
        }
        return true;
    }
};

struct StreamConfig {
    uint32_t width = kDefaultWidth;
    uint32_t height = kDefaultHeight;
    uint32_t fps = kDefaultFps;
    uint32_t bitrate = kDefaultBitrate;
    uint16_t port = kDefaultPort;
    uint64_t frames = 0;
    bool probeEncoder = false;
};

uint64_t NowMicros() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

bool IsValidFrameRingHeader(const UsbMonitorFrameRing::FrameRing* ring) {
    return ring != nullptr &&
        ring->magic == UsbMonitorFrameRing::kMagic &&
        ring->version == UsbMonitorFrameRing::kVersion &&
        ring->headerSize == offsetof(UsbMonitorFrameRing::FrameRing, slots) &&
        ring->slotSize == sizeof(UsbMonitorFrameRing::FrameSlot) &&
        ring->slotCount == UsbMonitorFrameRing::kSlotCount &&
        ring->maxWidth == UsbMonitorFrameRing::kMaxWidth &&
        ring->maxHeight == UsbMonitorFrameRing::kMaxHeight &&
        ring->ready == static_cast<std::int32_t>(UsbMonitorFrameRing::kReady);
}

class ScopedHandle final {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    void Reset(HANDLE handle = nullptr) {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

    HANDLE get() const { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

class FrameRingHost final {
public:
    FrameRingHost() {
        PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
        ULONG securityDescriptorLength = 0;
        constexpr wchar_t kSecurityDescriptor[] =
            L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;IU)";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                kSecurityDescriptor,
                SDDL_REVISION_1,
                &securityDescriptor,
                &securityDescriptorLength)) {
            const DWORD error = GetLastError();
            ThrowWin32(error, "ConvertStringSecurityDescriptorToSecurityDescriptorW");
        }

        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.lpSecurityDescriptor = securityDescriptor;
        securityAttributes.bInheritHandle = FALSE;

        const std::uint64_t mappingSize = sizeof(UsbMonitorFrameRing::FrameRing);
        HANDLE mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            &securityAttributes,
            PAGE_READWRITE,
            static_cast<DWORD>(mappingSize >> 32),
            static_cast<DWORD>(mappingSize & 0xffffffffu),
            UsbMonitorFrameRing::kMappingName);
        const DWORD mappingError = GetLastError();
        if (mapping != nullptr) {
            mapping_.Reset(mapping);
        }
        if (mapping == nullptr) {
            LocalFree(securityDescriptor);
            ThrowWin32(mappingError, "CreateFileMappingW(FrameRing)");
        }
        if (mappingError == ERROR_ALREADY_EXISTS) {
            LocalFree(securityDescriptor);
            throw std::runtime_error("CreateFileMappingW(FrameRing) found an existing named mapping");
        }

        HANDLE frameReadyEvent = CreateEventW(
            &securityAttributes,
            FALSE,
            FALSE,
            UsbMonitorFrameRing::kFrameReadyEventName);
        const DWORD eventError = GetLastError();
        if (frameReadyEvent != nullptr) {
            event_.Reset(frameReadyEvent);
        }
        LocalFree(securityDescriptor);
        if (frameReadyEvent == nullptr) {
            ThrowWin32(eventError, "CreateEventW(FrameReady)");
        }
        if (eventError == ERROR_ALREADY_EXISTS) {
            throw std::runtime_error("CreateEventW(FrameReady) found an existing named event");
        }

        ring_ = static_cast<UsbMonitorFrameRing::FrameRing*>(MapViewOfFile(
            mapping_.get(),
            FILE_MAP_READ | FILE_MAP_WRITE,
            0,
            0,
            sizeof(UsbMonitorFrameRing::FrameRing)));
        if (ring_ == nullptr) {
            ThrowWin32(GetLastError(), "MapViewOfFile(FrameRing)");
        }

        std::memset(ring_, 0, sizeof(*ring_));
        ring_->magic = UsbMonitorFrameRing::kMagic;
        ring_->version = UsbMonitorFrameRing::kVersion;
        ring_->headerSize = static_cast<std::uint32_t>(offsetof(UsbMonitorFrameRing::FrameRing, slots));
        ring_->slotSize = static_cast<std::uint32_t>(sizeof(UsbMonitorFrameRing::FrameSlot));
        ring_->slotCount = UsbMonitorFrameRing::kSlotCount;
        ring_->maxWidth = UsbMonitorFrameRing::kMaxWidth;
        ring_->maxHeight = UsbMonitorFrameRing::kMaxHeight;
        for (auto& slot : ring_->slots) {
            slot.state = UsbMonitorFrameRing::kSlotFree;
        }
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(&ring_->ready),
            static_cast<LONG>(UsbMonitorFrameRing::kReady));
        if (!IsValidFrameRingHeader(ring_)) {
            throw std::runtime_error("FrameRing initialization produced an invalid header");
        }
    }

    ~FrameRingHost() {
        if (ring_ != nullptr) {
            UnmapViewOfFile(ring_);
        }
    }

    FrameRingHost(const FrameRingHost&) = delete;
    FrameRingHost& operator=(const FrameRingHost&) = delete;

    UsbMonitorFrameRing::FrameRing* ring() const { return ring_; }
    HANDLE frameReadyEvent() const { return event_.get(); }

private:
    ScopedHandle mapping_;
    ScopedHandle event_;
    UsbMonitorFrameRing::FrameRing* ring_ = nullptr;
};

struct CapturedFrame {
    const std::uint8_t* nv12 = nullptr;
    std::size_t nv12Length = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint64_t frameId = 0;
    std::uint64_t captureTimestampUs = 0;
};

class FrameRingReader final {
public:
    enum class Result {
        Frame,
        Stopped,
        Disconnected,
    };

    explicit FrameRingReader(const FrameRingHost& host) : host_(host) {
        payload_.reserve(UsbMonitorFrameRing::kMaxPayloadBytes);
    }

    void BeginClient(std::uint32_t width, std::uint32_t height) {
        replayCachedFrame_ = hasCachedFrame_ && cachedWidth_ == width && cachedHeight_ == height;
    }

    Result WaitForFrame(SOCKET socket, CapturedFrame& output) {
        output = {};
        while (!g_stop.load()) {
            if (!IsSocketConnected(socket)) {
                return Result::Disconnected;
            }
            if (replayCachedFrame_) {
                replayCachedFrame_ = false;
                SetCachedFrame(output);
                return Result::Frame;
            }
            if (TryReadLatest(output)) {
                return Result::Frame;
            }
            if (terminal_) {
                return Result::Stopped;
            }

            const DWORD waitResult = WaitForSingleObject(host_.frameReadyEvent(), 100);
            if (waitResult == WAIT_FAILED) {
                SetError("WaitForSingleObject(FrameReady) failed (" + std::to_string(GetLastError()) + ")");
                return Result::Stopped;
            }
            if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_TIMEOUT) {
                SetError("WaitForSingleObject(FrameReady) returned an unexpected result");
                return Result::Stopped;
            }
        }
        return Result::Stopped;
    }

    const std::string& error() const { return error_; }

private:
    const FrameRingHost& host_;
    std::vector<std::uint8_t> payload_;
    std::string error_;
    bool terminal_ = false;
    bool hasCachedFrame_ = false;
    bool replayCachedFrame_ = false;
    std::uint32_t cachedWidth_ = 0;
    std::uint32_t cachedHeight_ = 0;
    std::uint32_t cachedStride_ = 0;
    std::uint64_t cachedFrameId_ = 0;
    std::uint64_t cachedCaptureTimestampUs_ = 0;

    static bool IsSocketConnected(SOCKET socket) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socket, &readSet);
        timeval timeout{};
        const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
        if (selected == SOCKET_ERROR) {
            return WSAGetLastError() == WSAEINTR;
        }
        if (selected == 0 || !FD_ISSET(socket, &readSet)) {
            return true;
        }
        char byte = 0;
        const int received = recv(socket, &byte, 1, MSG_PEEK);
        if (received == 0) {
            return false;
        }
        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            return error == WSAEWOULDBLOCK || error == WSAEINTR;
        }
        return true;
    }

    static LONG ReadState(const UsbMonitorFrameRing::FrameSlot& slot) {
        return slot.state;
    }

    static void FreeSlot(UsbMonitorFrameRing::FrameSlot& slot) {
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(&slot.state),
            static_cast<LONG>(UsbMonitorFrameRing::kSlotFree));
    }

    static bool ClaimReady(UsbMonitorFrameRing::FrameSlot& slot) {
        return InterlockedCompareExchange(
                   reinterpret_cast<volatile LONG*>(&slot.state),
                   static_cast<LONG>(UsbMonitorFrameRing::kSlotReading),
                   static_cast<LONG>(UsbMonitorFrameRing::kSlotReady)) ==
            static_cast<LONG>(UsbMonitorFrameRing::kSlotReady);
    }

    static bool IsValidSlot(const UsbMonitorFrameRing::FrameSlot& slot, std::size_t& payloadLength) {
        const std::uint32_t width = slot.width;
        const std::uint32_t height = slot.height;
        const std::uint32_t stride = slot.stride;
        if (slot.sequence == 0 || slot.frameId == 0 || slot.captureTimestampUs == 0 ||
            width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0 ||
            width > UsbMonitorFrameRing::kMaxWidth || height > UsbMonitorFrameRing::kMaxHeight ||
            stride < width || stride > UsbMonitorFrameRing::kMaxStride) {
            return false;
        }
        const std::size_t yBytes = static_cast<std::size_t>(stride) * height;
        const std::size_t uvBytes = static_cast<std::size_t>(stride) * (height / 2);
        const std::size_t expected = yBytes + uvBytes;
        if (expected > UsbMonitorFrameRing::kMaxPayloadBytes || slot.payloadLength != expected) {
            return false;
        }
        payloadLength = expected;
        return true;
    }

    void SetError(std::string error) {
        error_ = std::move(error);
        terminal_ = true;
    }

    void SetCachedFrame(CapturedFrame& output) {
        output.nv12 = payload_.data();
        output.nv12Length = payload_.size();
        output.width = cachedWidth_;
        output.height = cachedHeight_;
        output.stride = cachedStride_;
        output.frameId = cachedFrameId_;
        output.captureTimestampUs = cachedCaptureTimestampUs_;
    }

    bool TryReadLatest(CapturedFrame& output) {
        if (!IsValidFrameRingHeader(host_.ring())) {
            SetError("FrameRing header validation failed");
            return false;
        }

        auto* ring = host_.ring();
        for (unsigned attempt = 0; attempt < UsbMonitorFrameRing::kSlotCount * 2; ++attempt) {
            int newestIndex = -1;
            std::uint64_t newestSequence = 0;
            for (std::uint32_t index = 0; index < UsbMonitorFrameRing::kSlotCount; ++index) {
                auto& slot = ring->slots[index];
                if (ReadState(slot) == static_cast<LONG>(UsbMonitorFrameRing::kSlotReady) &&
                    (newestIndex < 0 || slot.sequence > newestSequence)) {
                    newestIndex = static_cast<int>(index);
                    newestSequence = slot.sequence;
                }
            }
            if (newestIndex < 0) {
                return false;
            }

            auto& selected = ring->slots[static_cast<std::size_t>(newestIndex)];
            if (!ClaimReady(selected)) {
                continue;
            }

            bool newerReadySlot = false;
            for (std::uint32_t index = 0; index < UsbMonitorFrameRing::kSlotCount; ++index) {
                const auto& slot = ring->slots[index];
                if (&slot != &selected &&
                    ReadState(slot) == static_cast<LONG>(UsbMonitorFrameRing::kSlotReady) &&
                    slot.sequence > selected.sequence) {
                    newerReadySlot = true;
                    break;
                }
            }
            if (newerReadySlot) {
                FreeSlot(selected);
                continue;
            }

            for (std::uint32_t index = 0; index < UsbMonitorFrameRing::kSlotCount; ++index) {
                auto& slot = ring->slots[index];
                if (&slot != &selected &&
                    ReadState(slot) == static_cast<LONG>(UsbMonitorFrameRing::kSlotReady) &&
                    slot.sequence < selected.sequence) {
                    InterlockedCompareExchange(
                        reinterpret_cast<volatile LONG*>(&slot.state),
                        static_cast<LONG>(UsbMonitorFrameRing::kSlotFree),
                        static_cast<LONG>(UsbMonitorFrameRing::kSlotReady));
                }
            }

            std::size_t payloadLength = 0;
            if (!IsValidSlot(selected, payloadLength)) {
                FreeSlot(selected);
                std::cerr << "[WARN] rejected corrupt FrameRing slot\n";
                return false;
            }

            payload_.resize(payloadLength);
            std::memcpy(payload_.data(), selected.payload, payloadLength);
            cachedWidth_ = selected.width;
            cachedHeight_ = selected.height;
            cachedStride_ = selected.stride;
            cachedFrameId_ = selected.frameId;
            cachedCaptureTimestampUs_ = selected.captureTimestampUs;
            hasCachedFrame_ = true;
            SetCachedFrame(output);
            FreeSlot(selected);
            return true;
        }
        return false;
    }
};

using NvencStatus = std::uint32_t;
using NvencInputPtr = void*;
using NvencOutputPtr = void*;

constexpr NvencStatus kNvencSuccess = 0;
constexpr NvencStatus kNvencNeedMoreInput = 17;
constexpr NvencStatus kNvencEncoderBusy = 18;
constexpr std::uint32_t kNvencApiVersion = 13u;
constexpr std::uint32_t NvencStructVersion(std::uint32_t version) {
    return kNvencApiVersion | (version << 16) | (7u << 28);
}

const GUID kNvencCodecH264 =
    {0x6bc82762, 0x4e63, 0x4ca4, {0xaa, 0x85, 0x1e, 0x50, 0xf3, 0x21, 0xf6, 0xbf}};
const GUID kNvencH264ProfileBaseline =
    {0x0727bcaa, 0x78c4, 0x4c83, {0x8c, 0x2f, 0xef, 0x3d, 0xff, 0x26, 0x7c, 0x6a}};
const GUID kNvencPresetLowLatencyHp =
    {0x67082a44, 0x4bad, 0x48fa, {0x98, 0xea, 0x93, 0x05, 0x6d, 0x15, 0x0a, 0x58}};

struct NvencConfig;

struct NvencMeHintCounts {
    std::uint32_t numCandsPerBlk16x16 : 4;
    std::uint32_t numCandsPerBlk16x8 : 4;
    std::uint32_t numCandsPerBlk8x16 : 4;
    std::uint32_t numCandsPerBlk8x8 : 4;
    std::uint32_t reserved : 16;
    std::uint32_t reserved1[3];
};

struct NvencRcParams {
    std::uint32_t version;
    std::uint32_t rateControlMode;
    std::uint32_t constQp[3];
    std::uint32_t averageBitRate;
    std::uint32_t maxBitRate;
    std::uint32_t vbvBufferSize;
    std::uint32_t vbvInitialDelay;
    std::uint32_t enableMinQp : 1;
    std::uint32_t enableMaxQp : 1;
    std::uint32_t enableInitialRcQp : 1;
    std::uint32_t enableAq : 1;
    std::uint32_t reservedBitField1 : 1;
    std::uint32_t enableLookahead : 1;
    std::uint32_t disableIadapt : 1;
    std::uint32_t disableBadapt : 1;
    std::uint32_t enableTemporalAq : 1;
    std::uint32_t zeroReorderDelay : 1;
    std::uint32_t enableNonRefP : 1;
    std::uint32_t strictGopTarget : 1;
    std::uint32_t aqStrength : 4;
    std::uint32_t enableExtLookahead : 1;
    std::uint32_t reservedBitFields : 15;
    std::uint32_t minQp[3];
    std::uint32_t maxQp[3];
    std::uint32_t initialRcQp[3];
    std::uint32_t temporallayerIdxMask;
    std::uint8_t temporalLayerQp[8];
    std::uint8_t targetQuality;
    std::uint8_t targetQualityLsb;
    std::uint16_t lookaheadDepth;
    std::uint8_t lowDelayKeyFrameScale;
    std::int8_t yDcQpIndexOffset;
    std::int8_t uDcQpIndexOffset;
    std::int8_t vDcQpIndexOffset;
    std::uint32_t qpMapMode;
    std::uint32_t multiPass;
    std::uint32_t alphaLayerBitrateRatio;
    std::int8_t cbQpIndexOffset;
    std::int8_t crQpIndexOffset;
    std::uint16_t reserved2;
    std::uint32_t lookaheadLevel;
    std::uint8_t viewBitrateRatios[7];
    std::uint8_t reserved3;
    std::uint32_t reserved1;
};

struct NvencConfig {
    std::uint32_t version;
    GUID profileGuid;
    std::uint32_t gopLength;
    std::int32_t frameIntervalP;
    std::uint32_t monoChromeEncoding;
    std::uint32_t frameFieldMode;
    std::uint32_t mvPrecision;
    NvencRcParams rcParams;
    std::uint32_t encodeCodecConfig[320];
    std::uint32_t reserved[278];
    void* reserved2[64];
};

struct NvencPresetConfig {
    std::uint32_t version;
    std::uint32_t reserved;
    NvencConfig presetCfg;
    std::uint32_t reserved1[256];
    void* reserved2[64];
};

struct NvencOpenSessionParams {
    std::uint32_t version;
    std::uint32_t deviceType;
    void* device;
    void* reserved;
    std::uint32_t apiVersion;
    std::uint32_t reserved1[253];
    void* reserved2[64];
};

struct NvencInitializeParams {
    std::uint32_t version;
    GUID encodeGuid;
    GUID presetGuid;
    std::uint32_t encodeWidth;
    std::uint32_t encodeHeight;
    std::uint32_t darWidth;
    std::uint32_t darHeight;
    std::uint32_t frameRateNum;
    std::uint32_t frameRateDen;
    std::uint32_t enableEncodeAsync;
    std::uint32_t enablePtd;
    std::uint32_t reportSliceOffsets : 1;
    std::uint32_t enableSubFrameWrite : 1;
    std::uint32_t enableExternalMeHints : 1;
    std::uint32_t enableMeOnlyMode : 1;
    std::uint32_t enableWeightedPrediction : 1;
    std::uint32_t splitEncodeMode : 4;
    std::uint32_t enableOutputInVidmem : 1;
    std::uint32_t enableReconFrameOutput : 1;
    std::uint32_t enableOutputStats : 1;
    std::uint32_t enableUniDirectionalB : 1;
    std::uint32_t reservedBitFields : 19;
    std::uint32_t privDataSize;
    std::uint32_t reserved;
    void* privData;
    NvencConfig* encodeConfig;
    std::uint32_t maxEncodeWidth;
    std::uint32_t maxEncodeHeight;
    NvencMeHintCounts maxMeHintCountsPerBlock[2];
    std::uint32_t tuningInfo;
    std::uint32_t bufferFormat;
    std::uint32_t numStateBuffers;
    std::uint32_t outputStatsLevel;
    std::uint32_t reserved1[284];
    void* reserved2[64];
};

struct NvencCreateInputBuffer {
    std::uint32_t version;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t memoryHeap;
    std::uint32_t bufferFmt;
    std::uint32_t reserved;
    NvencInputPtr inputBuffer;
    void* pSysMemBuffer;
    std::uint32_t reserved1[57];
    void* reserved2[63];
};

struct NvencCreateBitstreamBuffer {
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t memoryHeap;
    std::uint32_t reserved;
    NvencOutputPtr bitstreamBuffer;
    void* bitstreamBufferPtr;
    std::uint32_t reserved1[58];
    void* reserved2[64];
};

struct NvencLockInputBuffer {
    std::uint32_t version;
    std::uint32_t doNotWait : 1;
    std::uint32_t reservedBitFields : 31;
    NvencInputPtr inputBuffer;
    void* bufferDataPtr;
    std::uint32_t pitch;
    std::uint32_t reserved1[251];
    void* reserved2[64];
};

struct NvencLockBitstream {
    std::uint32_t version;
    std::uint32_t doNotWait : 1;
    std::uint32_t ltrFrame : 1;
    std::uint32_t reservedBitFields : 30;
    void* outputBitstream;
    std::uint32_t* sliceOffsets;
    std::uint32_t frameIdx;
    std::uint32_t hwEncodeStatus;
    std::uint32_t numSlices;
    std::uint32_t bitstreamSizeInBytes;
    std::uint64_t outputTimeStamp;
    std::uint64_t outputDuration;
    void* bitstreamBufferPtr;
    std::uint32_t pictureType;
    std::uint32_t pictureStruct;
    std::uint32_t frameAvgQp;
    std::uint32_t frameSatd;
    std::uint32_t ltrFrameIdx;
    std::uint32_t ltrFrameBitmap;
    std::uint32_t reserved[236];
    void* reserved2[64];
};

struct NvencCodecPicParams {
    std::uint32_t reserved[256];
};

struct NvencPicParams {
    std::uint32_t version;
    std::uint32_t inputWidth;
    std::uint32_t inputHeight;
    std::uint32_t inputPitch;
    std::uint32_t encodePicFlags;
    std::uint32_t frameIdx;
    std::uint64_t inputTimeStamp;
    std::uint64_t inputDuration;
    NvencInputPtr inputBuffer;
    NvencOutputPtr outputBitstream;
    void* completionEvent;
    std::uint32_t bufferFmt;
    std::uint32_t pictureStruct;
    std::uint32_t pictureType;
    NvencCodecPicParams codecPicParams;
    NvencMeHintCounts meHintCountsPerBlock[2];
    void* meExternalHints;
    std::uint32_t reserved1[6];
    void* reserved2[2];
    std::int8_t* qpDeltaMap;
    std::uint32_t qpDeltaMapSize;
    std::uint32_t reservedBitFields;
    std::uint16_t meHintRefPicDist[2];
    std::uint32_t reserved3[286];
    void* reserved4[60];
};

using NvencCreateInstanceFn = NvencStatus(__stdcall*)(void* functionList);
using NvencGetMaxVersionFn = NvencStatus(__stdcall*)(std::uint32_t* version);
using NvencOpenSessionExFn = NvencStatus(__stdcall*)(NvencOpenSessionParams*, void**);
using NvencGetPresetConfigFn = NvencStatus(__stdcall*)(void*, GUID, GUID, NvencPresetConfig*);
using NvencInitializeFn = NvencStatus(__stdcall*)(void*, NvencInitializeParams*);
using NvencCreateInputFn = NvencStatus(__stdcall*)(void*, NvencCreateInputBuffer*);
using NvencDestroyInputFn = NvencStatus(__stdcall*)(void*, NvencInputPtr);
using NvencCreateBitstreamFn = NvencStatus(__stdcall*)(void*, NvencCreateBitstreamBuffer*);
using NvencDestroyBitstreamFn = NvencStatus(__stdcall*)(void*, NvencOutputPtr);
using NvencEncodePictureFn = NvencStatus(__stdcall*)(void*, NvencPicParams*);
using NvencLockBitstreamFn = NvencStatus(__stdcall*)(void*, NvencLockBitstream*);
using NvencUnlockBitstreamFn = NvencStatus(__stdcall*)(void*, NvencOutputPtr);
using NvencLockInputFn = NvencStatus(__stdcall*)(void*, NvencLockInputBuffer*);
using NvencUnlockInputFn = NvencStatus(__stdcall*)(void*, NvencInputPtr);
using NvencDestroyEncoderFn = NvencStatus(__stdcall*)(void*);

struct NvencFunctionList {
    std::uint32_t version;
    std::uint32_t reserved;
    void* nvEncOpenEncodeSession;
    void* nvEncGetEncodeGuidCount;
    void* nvEncGetEncodeProfileGuidCount;
    void* nvEncGetEncodeProfileGuids;
    void* nvEncGetEncodeGuids;
    void* nvEncGetInputFormatCount;
    void* nvEncGetInputFormats;
    void* nvEncGetEncodeCaps;
    void* nvEncGetEncodePresetCount;
    void* nvEncGetEncodePresetGuids;
    void* nvEncGetEncodePresetConfig;
    void* nvEncInitializeEncoder;
    void* nvEncCreateInputBuffer;
    void* nvEncDestroyInputBuffer;
    void* nvEncCreateBitstreamBuffer;
    void* nvEncDestroyBitstreamBuffer;
    void* nvEncEncodePicture;
    void* nvEncLockBitstream;
    void* nvEncUnlockBitstream;
    void* nvEncLockInputBuffer;
    void* nvEncUnlockInputBuffer;
    void* nvEncGetEncodeStats;
    void* nvEncGetSequenceParams;
    void* nvEncRegisterAsyncEvent;
    void* nvEncUnregisterAsyncEvent;
    void* nvEncMapInputResource;
    void* nvEncUnmapInputResource;
    void* nvEncDestroyEncoder;
    void* nvEncInvalidateRefFrames;
    void* nvEncOpenEncodeSessionEx;
    void* nvEncRegisterResource;
    void* nvEncUnregisterResource;
    void* nvEncReconfigureEncoder;
    void* reserved1;
    void* nvEncCreateMvBuffer;
    void* nvEncDestroyMvBuffer;
    void* nvEncRunMotionEstimationOnly;
    void* reserved2[281];
};

std::string NvencStatusText(NvencStatus status) {
    switch (status) {
    case kNvencSuccess: return "success";
    case 8: return "invalid parameter";
    case 9: return "invalid call";
    case 10: return "out of memory";
    case 11: return "encoder not initialized";
    case 12: return "unsupported parameter";
    case 15: return "invalid version";
    case 17: return "need more input";
    case 18: return "encoder busy";
    case 20: return "generic failure";
    default: return "status " + std::to_string(status);
    }
}

void CheckNvenc(NvencStatus status, const char* operation) {
    if (status != kNvencSuccess) {
        throw std::runtime_error(std::string(operation) + " failed (" + NvencStatusText(status) + ")");
    }
}

class NvencH264Encoder final {
public:
    explicit NvencH264Encoder(const StreamConfig& config) : config_(config) {}

    ~NvencH264Encoder() {
        Reset();
    }

    void Initialize() {
        module_ = LoadLibraryW(L"nvEncodeAPI64.dll");
        if (module_ == nullptr) {
            throw std::runtime_error("nvEncodeAPI64.dll not found");
        }
        auto createInstance = reinterpret_cast<NvencCreateInstanceFn>(
            GetProcAddress(module_, "NvEncodeAPICreateInstance"));
        auto getMaxVersion = reinterpret_cast<NvencGetMaxVersionFn>(
            GetProcAddress(module_, "NvEncodeAPIGetMaxSupportedVersion"));
        if (createInstance == nullptr) {
            throw std::runtime_error("NvEncodeAPICreateInstance export not found");
        }
        if (getMaxVersion != nullptr) {
            std::uint32_t maxVersion = 0;
            CheckNvenc(getMaxVersion(&maxVersion), "NvEncodeAPIGetMaxSupportedVersion");
            std::cout << "[ENCODER] NVENC API max version: 0x" << std::hex << maxVersion << std::dec << '\n';
        }

        api_.version = NvencStructVersion(2);
        CheckNvenc(createInstance(&api_), "NvEncodeAPICreateInstance");
        auto openSession = Function<NvencOpenSessionExFn>(api_.nvEncOpenEncodeSessionEx);
        auto getPresetConfig = Function<NvencGetPresetConfigFn>(api_.nvEncGetEncodePresetConfig);
        auto initializeEncoder = Function<NvencInitializeFn>(api_.nvEncInitializeEncoder);
        auto createInput = Function<NvencCreateInputFn>(api_.nvEncCreateInputBuffer);
        auto createBitstream = Function<NvencCreateBitstreamFn>(api_.nvEncCreateBitstreamBuffer);
        if (openSession == nullptr || getPresetConfig == nullptr || initializeEncoder == nullptr ||
            createInput == nullptr || createBitstream == nullptr) {
            throw std::runtime_error("NvEncodeAPI function list is incomplete");
        }

        CreateDevice();
        NvencOpenSessionParams openParams{};
        openParams.version = NvencStructVersion(1);
        openParams.deviceType = 0;
        openParams.device = d3dDevice_.Get();
        openParams.apiVersion = kNvencApiVersion;
        CheckNvenc(openSession(&openParams, &session_), "NvEncOpenEncodeSessionEx");

        NvencPresetConfig preset{};
        preset.version = NvencStructVersion(5) | (1u << 31);
        preset.presetCfg.version = NvencStructVersion(9) | (1u << 31);
        CheckNvenc(
            getPresetConfig(session_, kNvencCodecH264, kNvencPresetLowLatencyHp, &preset),
            "NvEncGetEncodePresetConfig");
        NvencConfig encodeConfig = preset.presetCfg;
        encodeConfig.profileGuid = kNvencH264ProfileBaseline;
        encodeConfig.gopLength = std::max<std::uint32_t>(1, config_.fps / 4);
        encodeConfig.frameIntervalP = 1;
        encodeConfig.rcParams.rateControlMode = 2;
        encodeConfig.rcParams.averageBitRate = config_.bitrate;
        encodeConfig.rcParams.maxBitRate = config_.bitrate;
        encodeConfig.rcParams.vbvBufferSize = 0;
        encodeConfig.rcParams.vbvInitialDelay = 0;
        encodeConfig.rcParams.enableLookahead = 0;
        encodeConfig.rcParams.zeroReorderDelay = 1;
        encodeConfig.rcParams.disableIadapt = 1;
        encodeConfig.rcParams.disableBadapt = 1;

        NvencInitializeParams initializeParams{};
        initializeParams.version = NvencStructVersion(7) | (1u << 31);
        initializeParams.encodeGuid = kNvencCodecH264;
        initializeParams.presetGuid = kNvencPresetLowLatencyHp;
        initializeParams.encodeWidth = config_.width;
        initializeParams.encodeHeight = config_.height;
        initializeParams.darWidth = config_.width;
        initializeParams.darHeight = config_.height;
        initializeParams.frameRateNum = config_.fps;
        initializeParams.frameRateDen = 1;
        initializeParams.enablePtd = 1;
        initializeParams.maxEncodeWidth = config_.width;
        initializeParams.maxEncodeHeight = config_.height;
        initializeParams.bufferFormat = 1;
        initializeParams.encodeConfig = &encodeConfig;
        CheckNvenc(initializeEncoder(session_, &initializeParams), "NvEncInitializeEncoder");

        NvencCreateInputBuffer inputParams{};
        inputParams.version = NvencStructVersion(2);
        inputParams.width = config_.width;
        inputParams.height = config_.height;
        inputParams.bufferFmt = 1;
        CheckNvenc(createInput(session_, &inputParams), "NvEncCreateInputBuffer");
        inputBuffer_ = inputParams.inputBuffer;

        NvencCreateBitstreamBuffer bitstreamParams{};
        bitstreamParams.version = NvencStructVersion(1);
        CheckNvenc(createBitstream(session_, &bitstreamParams), "NvEncCreateBitstreamBuffer");
        bitstreamBuffer_ = bitstreamParams.bitstreamBuffer;
        std::cout << "[ENCODER] hardware: NVIDIA NVENC (sync)\n";
    }

    bool EncodeFrame(PacketWriter& writer, const CapturedFrame& frame, std::uint64_t frameIndex) {
        auto lockInput = Function<NvencLockInputFn>(api_.nvEncLockInputBuffer);
        auto unlockInput = Function<NvencUnlockInputFn>(api_.nvEncUnlockInputBuffer);
        auto encodePicture = Function<NvencEncodePictureFn>(api_.nvEncEncodePicture);
        auto lockBitstream = Function<NvencLockBitstreamFn>(api_.nvEncLockBitstream);
        auto unlockBitstream = Function<NvencUnlockBitstreamFn>(api_.nvEncUnlockBitstream);
        NvencLockInputBuffer inputLock{};
        inputLock.version = NvencStructVersion(1);
        inputLock.inputBuffer = inputBuffer_;
        CheckNvenc(lockInput(session_, &inputLock), "NvEncLockInputBuffer");
        if (inputLock.bufferDataPtr == nullptr || inputLock.pitch < frame.width) {
            unlockInput(session_, inputBuffer_);
            throw std::runtime_error("NvEncLockInputBuffer returned invalid pitch");
        }
        auto* destination = static_cast<std::uint8_t*>(inputLock.bufferDataPtr);
        for (std::uint32_t row = 0; row < frame.height; ++row) {
            std::memcpy(destination + static_cast<std::size_t>(row) * inputLock.pitch,
                        frame.nv12 + static_cast<std::size_t>(row) * frame.stride,
                        frame.width);
        }
        const auto* sourceUv = frame.nv12 + static_cast<std::size_t>(frame.stride) * frame.height;
        auto* destinationUv = destination + static_cast<std::size_t>(inputLock.pitch) * frame.height;
        for (std::uint32_t row = 0; row < frame.height / 2; ++row) {
            std::memcpy(destinationUv + static_cast<std::size_t>(row) * inputLock.pitch,
                        sourceUv + static_cast<std::size_t>(row) * frame.stride,
                        frame.width);
        }
        CheckNvenc(unlockInput(session_, inputBuffer_), "NvEncUnlockInputBuffer");

        pendingFrames_.push_back({frame.frameId, frame.captureTimestampUs});
        NvencPicParams picture{};
        picture.version = NvencStructVersion(4) | (1u << 31);
        picture.inputWidth = frame.width;
        picture.inputHeight = frame.height;
        picture.inputPitch = inputLock.pitch;
        picture.frameIdx = static_cast<std::uint32_t>(frameIndex);
        picture.inputTimeStamp = (frameIndex * 10'000'000ULL) / config_.fps;
        picture.inputBuffer = inputBuffer_;
        picture.outputBitstream = bitstreamBuffer_;
        picture.bufferFmt = 1;
        picture.pictureStruct = 1;
        picture.encodePicFlags = frameIndex == 0 ? 0x6 : 0;
        const NvencStatus encodeStatus = encodePicture(session_, &picture);
        if (encodeStatus == kNvencNeedMoreInput) {
            return true;
        }
        CheckNvenc(encodeStatus, "NvEncEncodePicture");

        NvencLockBitstream outputLock{};
        outputLock.version = NvencStructVersion(1);
        outputLock.outputBitstream = bitstreamBuffer_;
        CheckNvenc(lockBitstream(session_, &outputLock), "NvEncLockBitstream");
        bool sent = false;
        try {
            const FrameMetadata metadata = pendingFrames_.empty()
                ? FrameMetadata{frame.frameId, frame.captureTimestampUs}
                : pendingFrames_.front();
            if (!pendingFrames_.empty()) {
                pendingFrames_.pop_front();
            }
            sent = writer.SendAnnexB(
                metadata.frameId,
                metadata.timestampUs,
                static_cast<const std::uint8_t*>(outputLock.bitstreamBufferPtr),
                outputLock.bitstreamSizeInBytes);
        } catch (...) {
            unlockBitstream(session_, bitstreamBuffer_);
            throw;
        }
        CheckNvenc(unlockBitstream(session_, bitstreamBuffer_), "NvEncUnlockBitstream");
        return sent;
    }

    bool Drain(PacketWriter&) { return pendingFrames_.empty(); }
    void Flush() {}

private:
    struct FrameMetadata {
        std::uint64_t frameId;
        std::uint64_t timestampUs;
    };

    template <typename FunctionType>
    static FunctionType Function(void* value) {
        return reinterpret_cast<FunctionType>(value);
    }

    void CreateDevice() {
        ComPtr<IDXGIFactory1> factory;
        CheckHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1 for NVENC");
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 selected{};
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT result = factory->EnumAdapters1(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            CheckHr(result, "EnumAdapters1 for NVENC");
            DXGI_ADAPTER_DESC1 description{};
            CheckHr(candidate->GetDesc1(&description), "GetDesc1 for NVENC");
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && description.VendorId == 0x10DE) {
                adapter = candidate;
                selected = description;
                break;
            }
        }
        if (!adapter) {
            throw std::runtime_error("NVIDIA adapter not found");
        }
        D3D_FEATURE_LEVEL featureLevel{};
        CheckHr(D3D11CreateDevice(
                    adapter.Get(),
                    D3D_DRIVER_TYPE_UNKNOWN,
                    nullptr,
                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                    nullptr,
                    0,
                    D3D11_SDK_VERSION,
                    &d3dDevice_,
                    &featureLevel,
                    &d3dContext_),
                "D3D11CreateDevice for NVENC");
        std::cout << "[ENCODER] NVENC adapter: "
                  << WideToUtf8(selected.Description, static_cast<UINT32>(wcslen(selected.Description))) << '\n';
    }

    static std::string WideToUtf8(const wchar_t* value, UINT32 length) {
        if (value == nullptr || length == 0) {
            return {};
        }
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) {
            return {};
        }
        std::string result(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length), result.data(), bytes, nullptr, nullptr);
        return result;
    }

    void Reset() {
        if (session_ != nullptr && inputBuffer_ != nullptr && api_.nvEncDestroyInputBuffer != nullptr) {
            Function<NvencDestroyInputFn>(api_.nvEncDestroyInputBuffer)(session_, inputBuffer_);
        }
        if (session_ != nullptr && bitstreamBuffer_ != nullptr && api_.nvEncDestroyBitstreamBuffer != nullptr) {
            Function<NvencDestroyBitstreamFn>(api_.nvEncDestroyBitstreamBuffer)(session_, bitstreamBuffer_);
        }
        if (session_ != nullptr && api_.nvEncDestroyEncoder != nullptr) {
            Function<NvencDestroyEncoderFn>(api_.nvEncDestroyEncoder)(session_);
        }
        session_ = nullptr;
        inputBuffer_ = nullptr;
        bitstreamBuffer_ = nullptr;
        d3dContext_.Reset();
        d3dDevice_.Reset();
        if (module_ != nullptr) {
            FreeLibrary(module_);
            module_ = nullptr;
        }
        api_ = {};
        pendingFrames_.clear();
    }

    StreamConfig config_;
    HMODULE module_ = nullptr;
    NvencFunctionList api_{};
    void* session_ = nullptr;
    NvencInputPtr inputBuffer_ = nullptr;
    NvencOutputPtr bitstreamBuffer_ = nullptr;
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    std::deque<FrameMetadata> pendingFrames_;
};

class H264Encoder {
public:
    explicit H264Encoder(const StreamConfig& config) : config_(config) {}

    void Initialize() {
        ComPtr<IMFMediaType> outputType;
        CheckHr(MFCreateMediaType(&outputType), "MFCreateMediaType(output)");
        CheckHr(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "set output major type");
        CheckHr(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "set output subtype");
        CheckHr(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, config_.width, config_.height), "set output frame size");
        CheckHr(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, config_.fps, 1), "set output frame rate");
        CheckHr(MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "set output pixel aspect ratio");
        CheckHr(outputType->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate), "set output bitrate");
        CheckHr(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "set output interlace mode");
        CheckHr(outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base), "set H.264 profile");

        ComPtr<IMFMediaType> inputType;
        CheckHr(MFCreateMediaType(&inputType), "MFCreateMediaType(input)");
        CheckHr(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "set input major type");
        CheckHr(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "set input subtype");
        CheckHr(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, config_.width, config_.height), "set input frame size");
        CheckHr(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, config_.fps, 1), "set input frame rate");
        CheckHr(MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "set input pixel aspect ratio");
        CheckHr(inputType->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate), "set input bitrate");
        CheckHr(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "set input interlace mode");
        CheckHr(inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE), "set input independent samples");
        CheckHr(inputType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE), "set input fixed sample size");
        CheckHr(inputType->SetUINT32(MF_MT_SAMPLE_SIZE, config_.width * config_.height * 3 / 2), "set input sample size");
        CheckHr(inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, config_.width), "set input stride");
        outputType_ = outputType;
        inputType_ = inputType;

        if (!TryInitializeNvenc() && !TryInitializeHardware(outputType.Get(), inputType.Get())) {
            InitializeSoftware();
        }
    }

    bool EncodeFrame(PacketWriter& writer, const CapturedFrame& frame) {
        try {
            return EncodeFrameOnce(writer, frame);
        } catch (const std::exception& error) {
            if (!hardware_) {
                throw;
            }
            std::cerr << "[WARN] hardware encoder failed at runtime: " << error.what()
                      << "; falling back to software encoder\n";
            nvenc_.reset();
            InitializeSoftware();
            return EncodeFrameOnce(writer, frame);
        }
    }

    bool EncodeFrameOnce(PacketWriter& writer, const CapturedFrame& frame) {
        if (frame.nv12 == nullptr || frame.width != config_.width || frame.height != config_.height ||
            frame.stride < frame.width || (frame.width & 1u) != 0 || (frame.height & 1u) != 0) {
            throw std::runtime_error("FrameRing frame geometry does not match encoder configuration");
        }
        const size_t sourceYBytes = static_cast<size_t>(frame.stride) * frame.height;
        const size_t sourceBytes = sourceYBytes + static_cast<size_t>(frame.stride) * (frame.height / 2);
        const size_t frameBytes = static_cast<size_t>(config_.width) * config_.height * 3 / 2;
        if (frame.nv12Length != sourceBytes) {
            throw std::runtime_error("FrameRing frame payload length changed after validation");
        }

        if (nvenc_) {
            const bool sent = nvenc_->EncodeFrame(writer, frame, encodedFrameCount_);
            ++encodedFrameCount_;
            return sent;
        }

        ComPtr<IMFSample> sample = d3dDevice_
            ? CreateDxgiInputSample(frame)
            : CreateMemoryInputSample(frame, frameBytes, sourceYBytes);
        const LONGLONG sampleTime = static_cast<LONGLONG>((encodedFrameCount_ * 10'000'000ULL) / config_.fps);
        const LONGLONG sampleDuration = static_cast<LONGLONG>(10'000'000ULL / config_.fps);
        CheckHr(sample->SetSampleTime(sampleTime), "set sample timestamp");
        CheckHr(sample->SetSampleDuration(sampleDuration), "set sample duration");
        ++encodedFrameCount_;

        if (async_ && !WaitForInput(writer)) {
            return false;
        }

        pendingFrames_.push_back({frame.frameId, frame.captureTimestampUs});
        HRESULT inputResult = transform_->ProcessInput(0, sample.Get(), 0);
        if (async_) {
            CheckHr(inputResult, "hardware H.264 encoder ProcessInput");
            return DrainAvailableEvents(writer);
        }
        if (inputResult == MF_E_NOTACCEPTING) {
            if (!PumpOutput(writer)) {
                return false;
            }
            CheckHr(transform_->ProcessInput(0, sample.Get(), 0), "H.264 encoder ProcessInput after output");
        } else {
            CheckHr(inputResult, "H.264 encoder ProcessInput");
        }
        return PumpOutput(writer);
    }


    bool Drain(PacketWriter& writer) {
        if (nvenc_) {
            return nvenc_->Drain(writer);
        }
        CheckHr(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "H.264 encoder drain");
        if (async_) {
            drainComplete_ = false;
            while (!drainComplete_) {
                bool eventFound = false;
                if (!ProcessAsyncEvent(writer, 0, eventFound)) {
                    return false;
                }
            }
            return DrainAvailableEvents(writer);
        }
        return PumpOutput(writer);
    }

    void Flush() {
        if (nvenc_) {
            nvenc_->Flush();
            return;
        }
        if (transform_) {
            CheckHr(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0), "H.264 encoder flush");
        }
        pendingFrames_.clear();
    }

private:
    struct FrameMetadata {
        uint64_t frameId;
        uint64_t timestampUs;
    };

    StreamConfig config_;
    ComPtr<IMFMediaType> outputType_;
    ComPtr<IMFMediaType> inputType_;
    ComPtr<IMFTransform> transform_;
    ComPtr<IMFMediaEventGenerator> eventGenerator_;
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    ComPtr<IMFDXGIDeviceManager> dxgiDeviceManager_;
    std::unique_ptr<NvencH264Encoder> nvenc_;
    DWORD outputBufferSize_ = 0;
    std::deque<FrameMetadata> pendingFrames_;
    bool async_ = false;
    bool hardware_ = false;
    std::uint64_t encodedFrameCount_ = 0;
    bool drainComplete_ = false;
    unsigned pendingInputRequests_ = 0;

    static std::string WideToUtf8(const wchar_t* value, UINT32 length) {
        if (value == nullptr || length == 0) {
            return {};
        }
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) {
            return {};
        }
        std::string result(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length), result.data(), bytes, nullptr, nullptr);
        return result;
    }

    static std::string ActivationName(IMFActivate* activation) {
        wchar_t* value = nullptr;
        UINT32 length = 0;
        if (FAILED(activation->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &value, &length))) {
            return "hardware H.264 encoder";
        }
        const std::string result = WideToUtf8(value, length);
        CoTaskMemFree(value);
        return result.empty() ? "hardware H.264 encoder" : result;
    }


    void ResetTransform() {
        nvenc_.reset();
        eventGenerator_.Reset();
        transform_.Reset();
        dxgiDeviceManager_.Reset();
        d3dContext_.Reset();
        d3dDevice_.Reset();
        pendingFrames_.clear();
        outputBufferSize_ = 0;
        async_ = false;
        hardware_ = false;
        encodedFrameCount_ = 0;
        drainComplete_ = false;
        pendingInputRequests_ = 0;
    }

    ComPtr<IMFSample> CreateDxgiInputSample(const CapturedFrame& frame) {
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = frame.width;
        textureDesc.Height = frame.height;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_NV12;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_VIDEO_ENCODER;

        ComPtr<ID3D11Texture2D> texture;
        CheckHr(d3dDevice_->CreateTexture2D(&textureDesc, nullptr, &texture),
                "create hardware encoder NV12 texture");
        d3dContext_->UpdateSubresource(
            texture.Get(),
            0,
            nullptr,
            frame.nv12,
            frame.stride,
            static_cast<UINT>(frame.nv12Length));
        d3dContext_->Flush();

        ComPtr<IMFMediaBuffer> buffer;
        CheckHr(MFCreateDXGISurfaceBuffer(
                    __uuidof(ID3D11Texture2D),
                    texture.Get(),
                    0,
                    FALSE,
                    &buffer),
                "create hardware encoder DXGI buffer");
        ComPtr<IMFSample> sample;
        CheckHr(MFCreateSample(&sample), "create hardware encoder sample");
        CheckHr(sample->AddBuffer(buffer.Get()), "attach hardware encoder DXGI buffer");
        return sample;
    }

    static ComPtr<IMFSample> CreateMemoryInputSample(
        const CapturedFrame& frame,
        std::size_t frameBytes,
        std::size_t sourceYBytes) {
        ComPtr<IMFMediaBuffer> buffer;
        CheckHr(MFCreateMemoryBuffer(static_cast<DWORD>(frameBytes), &buffer), "MFCreateMemoryBuffer(NV12 frame)");
        BYTE* destination = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        CheckHr(buffer->Lock(&destination, &maxLength, &currentLength), "lock NV12 frame buffer");
        if (maxLength < frameBytes) {
            buffer->Unlock();
            throw std::runtime_error("H.264 encoder input buffer is smaller than configured NV12 frame");
        }
        for (uint32_t row = 0; row < frame.height; ++row) {
            std::memcpy(
                destination + static_cast<size_t>(row) * frame.width,
                frame.nv12 + static_cast<size_t>(row) * frame.stride,
                frame.width);
        }
        const BYTE* sourceUv = frame.nv12 + sourceYBytes;
        BYTE* destinationUv = destination + static_cast<size_t>(frame.width) * frame.height;
        for (uint32_t row = 0; row < frame.height / 2; ++row) {
            std::memcpy(
                destinationUv + static_cast<size_t>(row) * frame.width,
                sourceUv + static_cast<size_t>(row) * frame.stride,
                frame.width);
        }
        CheckHr(buffer->Unlock(), "unlock NV12 frame buffer");
        CheckHr(buffer->SetCurrentLength(static_cast<DWORD>(frameBytes)), "set NV12 frame length");
        ComPtr<IMFSample> sample;
        CheckHr(MFCreateSample(&sample), "MFCreateSample(NV12 frame)");
        CheckHr(sample->AddBuffer(buffer.Get()), "attach NV12 frame buffer");
        return sample;
    }

    void ConfigureHardwareDeviceManager(const std::string& encoderName) {
        ComPtr<IDXGIFactory1> factory;
        CheckHr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1 for hardware encoder");

        const bool preferIntel = encoderName.find("Intel") != std::string::npos;
        const bool preferNvidia = encoderName.find("NVIDIA") != std::string::npos;
        ComPtr<IDXGIAdapter1> selectedAdapter;
        DXGI_ADAPTER_DESC1 selectedDesc{};
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT enumResult = factory->EnumAdapters1(index, &adapter);
            if (enumResult == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            CheckHr(enumResult, "EnumAdapters1 for hardware encoder");
            DXGI_ADAPTER_DESC1 desc{};
            CheckHr(adapter->GetDesc1(&desc), "GetDesc1 for hardware encoder");
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }
            if (!selectedAdapter ||
                (preferIntel && desc.VendorId == 0x8086) ||
                (preferNvidia && desc.VendorId == 0x10DE)) {
                selectedAdapter = adapter;
                selectedDesc = desc;
            }
            if ((preferIntel && desc.VendorId == 0x8086) ||
                (preferNvidia && desc.VendorId == 0x10DE)) {
                break;
            }
        }
        if (!selectedAdapter) {
            throw std::runtime_error("no hardware DXGI adapter found for H.264 encoder");
        }

        const UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL featureLevel{};
        CheckHr(D3D11CreateDevice(
                    selectedAdapter.Get(),
                    D3D_DRIVER_TYPE_UNKNOWN,
                    nullptr,
                    deviceFlags,
                    nullptr,
                    0,
                    D3D11_SDK_VERSION,
                    &d3dDevice_,
                    &featureLevel,
                    &d3dContext_),
                "D3D11CreateDevice for hardware encoder");

        UINT resetToken = 0;
        CheckHr(MFCreateDXGIDeviceManager(&resetToken, &dxgiDeviceManager_),
                "MFCreateDXGIDeviceManager for hardware encoder");
        CheckHr(dxgiDeviceManager_->ResetDevice(d3dDevice_.Get(), resetToken),
                "reset hardware encoder DXGI device manager");
        CheckHr(transform_->ProcessMessage(
                    MFT_MESSAGE_SET_D3D_MANAGER,
                    reinterpret_cast<ULONG_PTR>(dxgiDeviceManager_.Get())),
                "set hardware encoder DXGI device manager");

        std::cout << "[ENCODER] D3D11 adapter: "
                  << WideToUtf8(selectedDesc.Description, static_cast<UINT32>(wcslen(selectedDesc.Description)))
                  << '\n';
    }

    bool TryInitializeHardware(IMFMediaType* outputType, IMFMediaType* inputType) {
        MFT_REGISTER_TYPE_INFO inputInfo{MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO outputInfo{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** rawActivations = nullptr;
        UINT32 activationCount = 0;
        const HRESULT enumResult = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputInfo,
            &outputInfo,
            &rawActivations,
            &activationCount);
        if (FAILED(enumResult)) {
            std::cerr << "[WARN] hardware H.264 encoder enumeration failed (0x"
                      << std::hex << static_cast<unsigned long>(enumResult) << std::dec << "): "
                      << HResultText(enumResult) << '\n';
            return false;
        }

        std::vector<ComPtr<IMFActivate>> activations;
        activations.reserve(activationCount);
        for (UINT32 index = 0; index < activationCount; ++index) {
            ComPtr<IMFActivate> activation;
            activation.Attach(rawActivations[index]);
            activations.push_back(std::move(activation));
        }
        CoTaskMemFree(rawActivations);

        for (const auto& activation : activations) {
            const std::string name = ActivationName(activation.Get());
            try {
                ResetTransform();
                CheckHr(
                    activation->ActivateObject(IID_PPV_ARGS(transform_.GetAddressOf())),
                    "activate hardware H.264 encoder");
                ConfigureTransform(outputType, inputType, name, true);
                return true;
            } catch (const std::exception& error) {
                std::cerr << "[WARN] hardware encoder rejected: " << name << ": " << error.what() << '\n';
            }
        }
        return false;
    }

    void InitializeSoftware() {
        ResetTransform();
        CheckHr(CoCreateInstance(
                    CLSID_MSH264EncoderMFT,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(transform_.GetAddressOf())),
                "CoCreateInstance(CLSID_MSH264EncoderMFT)");
        ConfigureTransform(outputType_.Get(), inputType_.Get(), "Microsoft H.264 software encoder", false);
    }

    bool TryInitializeNvenc() {
        try {
            auto encoder = std::make_unique<NvencH264Encoder>(config_);
            encoder->Initialize();
            nvenc_ = std::move(encoder);
            hardware_ = true;
            return true;
        } catch (const std::exception& error) {
            std::cerr << "[WARN] NVIDIA NVENC unavailable: " << error.what() << '\n';
            nvenc_.reset();
            return false;
        }
    }

    void ConfigureTransform(IMFMediaType* outputType, IMFMediaType* inputType, const std::string& name, bool hardware) {
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform_->GetAttributes(&attributes))) {
            if (!hardware) {
                attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
            }
            UINT32 asyncValue = FALSE;
            if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asyncValue)) && asyncValue != FALSE) {
                CheckHr(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "unlock asynchronous H.264 encoder");
                async_ = true;
            }
        }

        CheckHr(transform_->SetOutputType(0, outputType, 0), "H.264 encoder SetOutputType");

        if (hardware) {
            ComPtr<IMFAttributes> hardwareAttributes;
            UINT32 d3dAware = FALSE;
            if (SUCCEEDED(transform_->GetAttributes(&hardwareAttributes)) &&
                SUCCEEDED(hardwareAttributes->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware)) &&
                d3dAware != FALSE) {
                ConfigureHardwareDeviceManager(name);
            } else {
                std::cout << "[ENCODER] hardware MFT is not D3D11-aware; using system-memory input\n";
            }
        }

        CheckHr(transform_->SetInputType(0, inputType, 0), "H.264 encoder SetInputType");

        ComPtr<ICodecAPI> codecApi;
        const uint32_t keyframeInterval = std::max<uint32_t>(1, config_.fps / 4);
        if (SUCCEEDED(transform_.As(&codecApi))) {
            TrySetCodecBool(codecApi.Get(), CODECAPI_AVLowLatencyMode, true, "set encoder low-latency mode");
            if (!hardware) {
                TrySetCodecUInt32(codecApi.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR,
                                  "set CBR rate-control mode");
                TrySetCodecUInt32(codecApi.Get(), CODECAPI_AVEncCommonMeanBitRate, config_.bitrate,
                                  "set CBR mean bitrate");
                TrySetCodecUInt32(codecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0,
                                  "disable encoder B-frames");
                TrySetCodecUInt32(codecApi.Get(), CODECAPI_AVEncMPVGOPSize, keyframeInterval,
                                  "set encoder GOP size");
                TrySetCodecUInt32(codecApi.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 100,
                                  "prefer encoder speed");
                TrySetCodecBool(codecApi.Get(), CODECAPI_AVEncCommonRealTime, true, "set encoder real-time mode");
            }
        }

        CheckHr(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0),
                "H.264 encoder begin streaming");
        CheckHr(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0),
                "H.264 encoder start of stream");

        MFT_OUTPUT_STREAM_INFO streamInfo{};
        CheckHr(transform_->GetOutputStreamInfo(0, &streamInfo), "H.264 encoder GetOutputStreamInfo");
        outputBufferSize_ = std::max<DWORD>(streamInfo.cbSize, config_.width * config_.height);
        std::cout << "[ENCODER] output flags: 0x" << std::hex << streamInfo.dwFlags << std::dec << '\n';
        if (async_) {
            CheckHr(transform_.As(&eventGenerator_), "query hardware encoder event generator");
        }
        hardware_ = hardware;
        std::cout << "[ENCODER] " << (hardware ? "hardware" : "software") << ": " << name
                  << (async_ ? " (async)" : " (sync)") << '\n';
    }

    static void TrySetCodecBool(ICodecAPI* codecApi, const GUID& key, bool value, const char* operation) {
        const HRESULT supported = codecApi->IsSupported(&key);
        if (supported != S_OK) {
            std::cerr << "[WARN] " << operation << " skipped (codec property unsupported: 0x"
                      << std::hex << static_cast<unsigned long>(supported) << std::dec << ")\n";
            return;
        }

        const HRESULT modifiable = codecApi->IsModifiable(&key);
        if (modifiable != S_OK) {
            std::cerr << "[WARN] " << operation << " skipped (codec property not modifiable: 0x"
                      << std::hex << static_cast<unsigned long>(modifiable) << std::dec << ")\n";
            return;
        }

        VARIANT variant;
        VariantInit(&variant);
        variant.vt = VT_BOOL;
        variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        const HRESULT hr = codecApi->SetValue(&key, &variant);
        VariantClear(&variant);
        if (FAILED(hr)) {
            std::cerr << "[WARN] " << operation << " skipped (0x"
                      << std::hex << static_cast<unsigned long>(hr) << std::dec << "): "
                      << HResultText(hr) << '\n';
        }
    }

    static void TrySetCodecUInt32(ICodecAPI* codecApi, const GUID& key, uint32_t value, const char* operation) {
        VARIANT variant;
        VariantInit(&variant);
        variant.vt = VT_UI4;
        variant.ulVal = value;
        const HRESULT hr = codecApi->SetValue(&key, &variant);
        VariantClear(&variant);
        if (FAILED(hr)) {
            std::cerr << "[WARN] " << operation << " skipped (0x"
                      << std::hex << static_cast<unsigned long>(hr) << std::dec << "): "
                      << HResultText(hr) << '\n';
        }
    }

    bool WaitForInput(PacketWriter& writer) {
        while (pendingInputRequests_ == 0) {
            bool eventFound = false;
            if (!ProcessAsyncEvent(writer, 0, eventFound)) {
                return false;
            }
        }
        --pendingInputRequests_;
        return true;
    }

    bool DrainAvailableEvents(PacketWriter& writer) {
        while (true) {
            bool eventFound = false;
            if (!ProcessAsyncEvent(writer, MF_EVENT_FLAG_NO_WAIT, eventFound)) {
                return false;
            }
            if (!eventFound) {
                return true;
            }
        }
    }

    bool ProcessAsyncEvent(PacketWriter& writer, DWORD flags, bool& eventFound) {
        eventFound = false;
        ComPtr<IMFMediaEvent> event;
        const HRESULT eventResult = eventGenerator_->GetEvent(flags, &event);
        if (eventResult == MF_E_NO_EVENTS_AVAILABLE) {
            return true;
        }
        CheckHr(eventResult, "hardware encoder GetEvent");
        eventFound = true;

        HRESULT eventStatus = S_OK;
        CheckHr(event->GetStatus(&eventStatus), "hardware encoder event status");
        CheckHr(eventStatus, "hardware encoder event");

        MediaEventType eventType = MEUnknown;
        CheckHr(event->GetType(&eventType), "hardware encoder event type");
        if (eventType == METransformNeedInput) {
            ++pendingInputRequests_;
            return true;
        }
        if (eventType == METransformHaveOutput) {
            return PumpOutput(writer);
        }
        if (eventType == METransformDrainComplete) {
            drainComplete_ = true;
        }
        return true;
    }

    void RenegotiateOutputType() {
        for (DWORD typeIndex = 0;; ++typeIndex) {
            ComPtr<IMFMediaType> outputType;
            const HRESULT typeResult = transform_->GetOutputAvailableType(0, typeIndex, &outputType);
            if (typeResult == MF_E_NO_MORE_TYPES) {
                break;
            }
            CheckHr(typeResult, "H.264 encoder GetOutputAvailableType after stream change");

            GUID majorType{};
            GUID subtype{};
            if (FAILED(outputType->GetGUID(MF_MT_MAJOR_TYPE, &majorType)) ||
                FAILED(outputType->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
                majorType != MFMediaType_Video || subtype != MFVideoFormat_H264) {
                continue;
            }

            UINT32 offeredWidth = 0;
            UINT32 offeredHeight = 0;
            const HRESULT sizeResult = MFGetAttributeSize(
                outputType.Get(), MF_MT_FRAME_SIZE, &offeredWidth, &offeredHeight);
            if (SUCCEEDED(sizeResult) &&
                (offeredWidth != config_.width || offeredHeight != config_.height)) {
                std::cout << "[ENCODER] skip offered output " << offeredWidth << 'x' << offeredHeight << '\n';
                continue;
            }
            transform_->SetOutputType(0, nullptr, 0);
            if (SUCCEEDED(transform_->SetOutputType(0, outputType.Get(), 0))) {
                MFT_OUTPUT_STREAM_INFO streamInfo{};
                CheckHr(transform_->GetOutputStreamInfo(0, &streamInfo),
                        "H.264 encoder GetOutputStreamInfo after stream change");
                outputBufferSize_ = std::max<DWORD>(streamInfo.cbSize, config_.width * config_.height);
                std::cout << "[ENCODER] output stream renegotiated, flags: 0x"
                          << std::hex << streamInfo.dwFlags << std::dec << '\n';
                return;
            }
        }
        throw std::runtime_error("H.264 encoder offered no usable H.264 output type after stream change");
    }

    bool PumpOutput(PacketWriter& writer) {
        while (true) {
            MFT_OUTPUT_STREAM_INFO streamInfo{};
            CheckHr(transform_->GetOutputStreamInfo(0, &streamInfo), "H.264 encoder GetOutputStreamInfo");
            const bool provideSample =
                (streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            ComPtr<IMFSample> suppliedSample;
            if (!provideSample) {
                ComPtr<IMFMediaBuffer> outputBuffer;
                const DWORD size = std::max<DWORD>(streamInfo.cbSize, outputBufferSize_);
                CheckHr(MFCreateMemoryBuffer(size, &outputBuffer), "MFCreateMemoryBuffer(encoder output)");
                CheckHr(MFCreateSample(&suppliedSample), "MFCreateSample(encoder output)");
                CheckHr(suppliedSample->AddBuffer(outputBuffer.Get()), "attach encoder output buffer");
            }

            MFT_OUTPUT_DATA_BUFFER output{};
            output.dwStreamID = 0;
            output.pSample = suppliedSample.Get();
            DWORD status = 0;
            const HRESULT result = transform_->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents) {
                output.pEvents->Release();
                output.pEvents = nullptr;
            }
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return true;
            }
            if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
                RenegotiateOutputType();
                if (async_) {
                    return true;
                }
                continue;
            }
            CheckHr(result, "H.264 encoder ProcessOutput");

            ComPtr<IMFSample> outputSample;
            if (provideSample) {
                if (output.pSample == nullptr) {
                    throw std::runtime_error("H.264 encoder ProcessOutput returned no sample");
                }
                outputSample.Attach(output.pSample);
            } else {
                outputSample = suppliedSample;
            }
            if (!pendingFrames_.empty()) {
                const FrameMetadata metadata = pendingFrames_.front();
                pendingFrames_.pop_front();
                if (!SendSample(writer, outputSample.Get(), metadata)) {
                    return false;
                }
            } else if (!SendSample(writer, outputSample.Get(), {0, NowMicros()})) {
                return false;
            }
        }
    }

    static bool SendSample(PacketWriter& writer, IMFSample* sample, const FrameMetadata& metadata) {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr)) {
            ThrowHr(hr, "convert H.264 output to contiguous buffer");
        }
        BYTE* data = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        CheckHr(buffer->Lock(&data, &maxLength, &currentLength), "lock H.264 output buffer");
        const bool sent = writer.SendAnnexB(metadata.frameId, metadata.timestampUs, data, currentLength);
        const HRESULT unlockResult = buffer->Unlock();
        CheckHr(unlockResult, "unlock H.264 output buffer");
        return sent;
    }
};

void CloseSocket(std::atomic<SOCKET>& slot) {
    const SOCKET socket = slot.exchange(INVALID_SOCKET);
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }
}

void ConfigureClientSocket(SOCKET socket) {
    const int enabled = 1;
    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == SOCKET_ERROR) {
        const int code = WSAGetLastError();
        throw std::runtime_error("setsockopt(TCP_NODELAY) failed (" + std::to_string(code) + "): " + WsaText(code));
    }
    const int sendBufferBytes = kClientSendBufferBytes;
    if (setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBufferBytes), sizeof(sendBufferBytes)) == SOCKET_ERROR) {
        const int code = WSAGetLastError();
        throw std::runtime_error("setsockopt(SO_SNDBUF) failed (" + std::to_string(code) + "): " + WsaText(code));
    }
    const int sendTimeoutMs = kClientSendTimeoutMs;
    if (setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeoutMs), sizeof(sendTimeoutMs)) == SOCKET_ERROR) {
        const int code = WSAGetLastError();
        throw std::runtime_error("setsockopt(SO_SNDTIMEO) failed (" + std::to_string(code) + "): " + WsaText(code));
    }
}

BOOL WINAPI ConsoleControlHandler(DWORD controlType) {
    switch (controlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop.store(true);
        CloseSocket(g_client);
        CloseSocket(g_listener);
        return TRUE;
    default:
        return FALSE;
    }
}

bool SendHandshakeResponse(SOCKET socket, const StreamConfig& config, std::string& error) {
    const std::string response = "{\"codec\":\"h264\",\"width\":" + std::to_string(config.width) +
        ",\"height\":" + std::to_string(config.height) + ",\"fps\":" + std::to_string(config.fps) +
        ",\"bitrate\":" + std::to_string(config.bitrate) + "}\n";
    size_t offset = 0;
    while (offset < response.size()) {
        const int sent = send(socket, response.data() + offset, static_cast<int>(response.size() - offset), 0);
        if (sent == SOCKET_ERROR) {
            const int code = WSAGetLastError();
            error = "send(handshake response) failed (" + std::to_string(code) + "): " + WsaText(code);
            return false;
        }
        if (sent == 0) {
            error = "client disconnected while receiving handshake response";
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

void PrintUsage() {
    std::cout << "Usage: second-screen-host [options]\n"
              << "  --width N       encoded width (default 1920; supported: 1920 or 2560)\n"
              << "  --height N      encoded height (default 1200; supported: 1080, 1200, or 1600)\n"
              << "  --fps N         frame rate (default 60)\n"
              << "  --bitrate N     CBR bitrate in bits/sec (default 12000000)\n"
              << "  --port N        loopback TCP port (default 5000)\n"
              << "  --frames N      frames to capture, 0 means unlimited (default 0)\n"
              << "  --probe-encoder initialize and print the selected H.264 encoder\n"
              << "  --help          show this help\n";
}

bool ParseUnsigned(std::string_view value, uint64_t maximum, uint64_t& result) {
    if (value.empty()) {
        return false;
    }
    const char* first = value.data();
    const char* last = first + value.size();
    uint64_t parsed = 0;
    const auto conversion = std::from_chars(first, last, parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != last || parsed > maximum) {
        return false;
    }
    result = parsed;
    return true;
}

bool ParseCommandLine(int argc, char** argv, StreamConfig& config, bool& showHelp, std::string& error) {
    showHelp = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--help") {
            showHelp = true;
            continue;
        }
        if (argument == "--probe-encoder") {
            config.probeEncoder = true;
            continue;
        }
        std::string_view name;
        std::string_view value;
        const size_t equals = argument.find('=');
        if (equals == std::string_view::npos) {
            name = argument;
            if (i + 1 >= argc) {
                error = std::string(name) + " requires a value";
                return false;
            }
            value = argv[++i];
        } else {
            name = argument.substr(0, equals);
            value = argument.substr(equals + 1);
        }
        uint64_t parsed = 0;
        if (name == "--width") {
            if (!ParseUnsigned(value, 16384, parsed) || parsed == 0 || (parsed & 1) != 0) {
                error = "--width must be a positive even integer no greater than 16384";
                return false;
            }
            config.width = static_cast<uint32_t>(parsed);
        } else if (name == "--height") {
            if (!ParseUnsigned(value, 16384, parsed) || parsed == 0 || (parsed & 1) != 0) {
                error = "--height must be a positive even integer no greater than 16384";
                return false;
            }
            config.height = static_cast<uint32_t>(parsed);
        } else if (name == "--fps") {
            if (!ParseUnsigned(value, 240, parsed) || parsed == 0) {
                error = "--fps must be between 1 and 240";
                return false;
            }
            config.fps = static_cast<uint32_t>(parsed);
        } else if (name == "--bitrate") {
            if (!ParseUnsigned(value, std::numeric_limits<uint32_t>::max(), parsed) || parsed == 0) {
                error = "--bitrate must be a positive 32-bit integer";
                return false;
            }
            config.bitrate = static_cast<uint32_t>(parsed);
        } else if (name == "--port") {
            if (!ParseUnsigned(value, 65535, parsed) || parsed == 0) {
                error = "--port must be between 1 and 65535";
                return false;
            }
            config.port = static_cast<uint16_t>(parsed);
        } else if (name == "--frames") {
            if (!ParseUnsigned(value, std::numeric_limits<uint64_t>::max(), parsed)) {
                error = "--frames must be an unsigned 64-bit integer";
                return false;
            }
            config.frames = parsed;
        } else {
            error = "unknown option: " + std::string(name);
            return false;
        }
    }
    const bool supportedGeometry =
        (config.width == 1920 && (config.height == 1080 || config.height == 1200)) ||
        (config.width == 2560 && config.height == 1600);
    if (!supportedGeometry) {
        error = "--width/--height must select 1920x1080, 1920x1200, or 2560x1600";
        return false;
    }
    return true;
}

void StreamClient(
    SOCKET socket,
    const StreamConfig& config,
    FrameRingReader& reader,
    uint64_t& encodedFrames) {
    std::string handshakeLine;
    std::string handshakeError;
    if (!ReadHandshake(socket, handshakeLine, handshakeError)) {
        std::cerr << "[CLIENT] handshake read failed: " << handshakeError << '\n';
        return;
    }
    if (!IsValidUtf8(handshakeLine)) {
        std::cerr << "[CLIENT] handshake rejected: invalid UTF-8\n";
        return;
    }
    JsonParser::Handshake request;
    JsonParser parser(handshakeLine);
    if (!parser.ParseHandshake(request, handshakeError)) {
        std::cerr << "[CLIENT] handshake rejected: " << handshakeError << '\n';
        return;
    }
    reader.BeginClient(config.width, config.height);
    CapturedFrame firstFrame;
    const FrameRingReader::Result firstResult = reader.WaitForFrame(socket, firstFrame);
    if (firstResult != FrameRingReader::Result::Frame) {
        if (!reader.error().empty()) {
            std::cerr << "[CLIENT] frame ring stopped: " << reader.error() << '\n';
        } else if (firstResult == FrameRingReader::Result::Disconnected) {
            std::cerr << "[CLIENT] disconnected before first frame\n";
        }
        return;
    }
    StreamConfig effectiveConfig = config;
    if (firstFrame.width != config.width || firstFrame.height != config.height) {
        std::cerr << "[WARN] requested " << config.width << 'x' << config.height
                  << ", capture is " << firstFrame.width << 'x' << firstFrame.height
                  << "; using capture geometry\n";
        effectiveConfig.width = firstFrame.width;
        effectiveConfig.height = firstFrame.height;
    }
    H264Encoder encoder(effectiveConfig);
    encoder.Initialize();
    PacketWriter writer(socket);
    if (!SendHandshakeResponse(socket, effectiveConfig, handshakeError)) {
        std::cerr << "[CLIENT] handshake response failed: " << handshakeError << '\n';
        return;
    }
    std::cout << "[CLIENT] connected: " << request.device << " (requested " << request.width << 'x' << request.height
              << ", streaming " << effectiveConfig.width << 'x' << effectiveConfig.height << ")\n";
    if (!g_stop.load() && (config.frames == 0 || encodedFrames < config.frames)) {
        if (!encoder.EncodeFrame(writer, firstFrame)) {
            std::cerr << "[CLIENT] stream stopped: " << writer.error() << '\n';
            return;
        }
        ++encodedFrames;
    }
    while (!g_stop.load() && (config.frames == 0 || encodedFrames < config.frames)) {
        CapturedFrame frame;
        const FrameRingReader::Result result = reader.WaitForFrame(socket, frame);
        if (result != FrameRingReader::Result::Frame) {
            if (!reader.error().empty()) {
                std::cerr << "[CLIENT] frame ring stopped: " << reader.error() << '\n';
            } else if (result == FrameRingReader::Result::Disconnected) {
                std::cerr << "[CLIENT] disconnected while waiting for frame\n";
            }
            return;
        }
        if (!encoder.EncodeFrame(writer, frame)) {
            std::cerr << "[CLIENT] stream stopped: " << writer.error() << '\n';
            return;
        }
        ++encodedFrames;
    }
    if (!g_stop.load() && !encoder.Drain(writer)) {
        std::cerr << "[CLIENT] drain stopped: " << writer.error() << '\n';
        return;
    }
    encoder.Flush();
}
} // namespace

int main(int argc, char** argv) {
    StreamConfig config;
    bool showHelp = false;
    std::string commandLineError;
    if (!ParseCommandLine(argc, argv, config, showHelp, commandLineError)) {
        std::cerr << "[ERROR] " << commandLineError << '\n';
        PrintUsage();
        return 2;
    }
    if (showHelp) {
        PrintUsage();
        return 0;
    }

    if (!SetConsoleCtrlHandler(ConsoleControlHandler, TRUE)) {
        std::cerr << "[ERROR] SetConsoleCtrlHandler failed: " << GetLastError() << '\n';
        return 1;
    }

    HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        std::cerr << "[ERROR] CoInitializeEx failed (0x" << std::hex << static_cast<unsigned long>(comResult)
                  << std::dec << "): " << HResultText(comResult) << '\n';
        SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
        return 1;
    }
    bool comInitialized = SUCCEEDED(comResult);
    HRESULT mfResult = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(mfResult)) {
        if (comInitialized) CoUninitialize();
        std::cerr << "[ERROR] MFStartup failed (0x" << std::hex << static_cast<unsigned long>(mfResult)
                  << std::dec << "): " << HResultText(mfResult) << '\n';
        SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
        return 1;
    }

    if (config.probeEncoder) {
        int probeExitCode = 0;
        try {
            H264Encoder encoder(config);
            encoder.Initialize();
            const std::size_t frameBytes = static_cast<std::size_t>(config.width) * config.height * 3 / 2;
            std::vector<std::uint8_t> nv12(frameBytes, 128);
            std::fill(nv12.begin(), nv12.begin() + static_cast<std::size_t>(config.width) * config.height, 16);
            PacketWriter writer;
            for (std::uint64_t frameId = 1; frameId <= 8; ++frameId) {
                CapturedFrame frame{
                    nv12.data(),
                    nv12.size(),
                    config.width,
                    config.height,
                    config.width,
                    frameId,
                    NowMicros()};
                if (!encoder.EncodeFrame(writer, frame)) {
                    throw std::runtime_error("encoder probe output failed: " + writer.error());
                }
            }
            if (!encoder.Drain(writer)) {
                throw std::runtime_error("encoder probe drain failed");
            }
            std::cout << "[ENCODER] probe encoded 8 frames successfully\n";
        } catch (const std::exception& error) {
            std::cerr << "[ERROR] " << error.what() << '\n';
            probeExitCode = 1;
        }
        MFShutdown();
        if (comInitialized) {
            CoUninitialize();
        }
        SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
        return probeExitCode;
    }

    WSADATA wsaData{};
    int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaResult != 0) {
        MFShutdown();
        if (comInitialized) CoUninitialize();
        std::cerr << "[ERROR] WSAStartup failed (" << wsaResult << "): " << WsaText(wsaResult) << '\n';
        return 1;
    }

    int exitCode = 0;
    SOCKET listener = INVALID_SOCKET;
    try {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            const int code = WSAGetLastError();
            throw std::runtime_error("socket failed (" + std::to_string(code) + "): " + WsaText(code));
        }
        g_listener.store(listener);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(config.port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            const int code = WSAGetLastError();
            throw std::runtime_error("bind 127.0.0.1:" + std::to_string(config.port) + " failed (" + std::to_string(code) + "): " + WsaText(code));
        }
        if (listen(listener, 1) == SOCKET_ERROR) {
            const int code = WSAGetLastError();
            throw std::runtime_error("listen failed (" + std::to_string(code) + "): " + WsaText(code));
        }
        if (!EnablePrivilege(L"SeCreateGlobalPrivilege")) {
            throw std::runtime_error("Global frame ring requires SeCreateGlobalPrivilege; run the host elevated");
        }
        FrameRingHost frameRing;
        FrameRingReader frameReader(frameRing);
        uint64_t encodedFrames = 0;
        std::cout << "[LISTEN] 127.0.0.1:" << config.port << " " << config.width << 'x' << config.height
                  << " @ " << config.fps << " fps, " << config.bitrate << " bps\n";
        while (!g_stop.load() && (config.frames == 0 || encodedFrames < config.frames)) {
            sockaddr_in clientAddress{};
            int clientAddressLength = sizeof(clientAddress);
            SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&clientAddress), &clientAddressLength);
            if (client == INVALID_SOCKET) {
                if (g_stop.load()) {
                    break;
                }
                const int code = WSAGetLastError();
                std::cerr << "[ERROR] accept failed (" << code << "): " << WsaText(code) << '\n';
                exitCode = 1;
                break;
            }
            g_client.store(client);
            ConfigureClientSocket(client);
            StreamClient(client, config, frameReader, encodedFrames);
            CloseSocket(g_client);
        }
        CloseSocket(g_listener);
        listener = INVALID_SOCKET;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        exitCode = 1;
        if (listener != INVALID_SOCKET) {
            CloseSocket(g_listener);
            listener = INVALID_SOCKET;
        }
        CloseSocket(g_client);
    }

    WSACleanup();
    MFShutdown();
    if (comInitialized) {
        CoUninitialize();
    }
    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    return exitCode;
}
