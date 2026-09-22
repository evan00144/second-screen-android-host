#pragma once

#include <cstddef>
#include <cstdint>

namespace UsbMonitorFrameRing
{
constexpr std::uint32_t kMagic = 0x31465253; // "SRF1"
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kReady = 1;
constexpr std::uint32_t kSlotCount = 2;
constexpr std::uint32_t kMaxWidth = 2560;
constexpr std::uint32_t kMaxHeight = 1600;
constexpr std::uint32_t kMaxStride = kMaxWidth;
constexpr std::size_t kMaxPayloadBytes =
    static_cast<std::size_t>(kMaxStride) * kMaxHeight * 3 / 2;

constexpr std::int32_t kSlotFree = 0;
constexpr std::int32_t kSlotWriting = 1;
constexpr std::int32_t kSlotReady = 2;
constexpr std::int32_t kSlotReading = 3;

// Global is required because UMDF WUDFHost runs in session 0 while the host is interactive.
constexpr wchar_t kMappingName[] = L"Global\\UsbMonitorTransport.FrameRing.v1";
constexpr wchar_t kFrameReadyEventName[] = L"Global\\UsbMonitorTransport.FrameReady.v1";

struct alignas(64) FrameSlot
{
    volatile std::int32_t state{};
    std::uint32_t reserved0{};
    std::uint64_t sequence{};
    std::uint64_t frameId{};
    std::uint64_t captureTimestampUs{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride{};
    std::uint32_t payloadLength{};
    std::uint32_t reserved1[4]{};
    std::uint8_t payload[kMaxPayloadBytes]{};
};

struct alignas(64) FrameRing
{
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t headerSize{};
    std::uint32_t slotSize{};
    std::uint32_t slotCount{};
    std::uint32_t maxWidth{};
    std::uint32_t maxHeight{};
    volatile std::int32_t ready{};
    std::uint32_t reserved[8]{};
    FrameSlot slots[kSlotCount]{};
};

static_assert(sizeof(std::int32_t) == sizeof(long), "Frame-ring state must match Windows LONG.");
static_assert(offsetof(FrameRing, slots) % 64 == 0, "Frame slots must be cache-line aligned.");
static_assert(sizeof(FrameRing) < 0xFFFFFFFFull, "Frame-ring mapping must fit CreateFileMapping size.");

} // namespace UsbMonitorFrameRing
