#include "Driver.h"

#include <emmintrin.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

using namespace UsbMonitorIddCx;

namespace
{
using UsbMonitorFrameRing::FrameRing;
using UsbMonitorFrameRing::FrameSlot;

LONG ReadInterlockedState(volatile std::int32_t* state) noexcept
{
    return InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(state), 0, 0);
}

bool ValidateFrameRing(FrameRing* ring) noexcept
{
    if (ring == nullptr)
    {
        return false;
    }
    return ring->magic == UsbMonitorFrameRing::kMagic &&
           ring->version == UsbMonitorFrameRing::kVersion &&
           ring->headerSize == offsetof(FrameRing, slots) &&
           ring->slotSize == sizeof(FrameSlot) &&
           ring->slotCount == UsbMonitorFrameRing::kSlotCount &&
           ring->maxWidth == UsbMonitorFrameRing::kMaxWidth &&
           ring->maxHeight == UsbMonitorFrameRing::kMaxHeight &&
           ReadInterlockedState(&ring->ready) == UsbMonitorFrameRing::kReady;
}

std::uint8_t ClampByte(int value) noexcept
{
    return static_cast<std::uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
}

std::uint64_t QpcToMicroseconds(UINT64 qpc) noexcept
{
    LARGE_INTEGER frequency{};
    if (qpc == 0 || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    {
        return 0;
    }
    const std::uint64_t ticksPerSecond = static_cast<std::uint64_t>(frequency.QuadPart);
    const std::uint64_t seconds = qpc / ticksPerSecond;
    const std::uint64_t remainder = qpc % ticksPerSecond;
    if (seconds > (std::numeric_limits<std::uint64_t>::max)() / 1'000'000)
    {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    return seconds * 1'000'000 + (remainder * 1'000'000) / ticksPerSecond;
}

UINT64 QueryQpc() noexcept
{
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) ? static_cast<UINT64>(value.QuadPart) : 0;
}

std::uint64_t QpcDeltaToMicroseconds(UINT64 start, UINT64 end) noexcept
{
    if (start == 0 || end <= start)
    {
        return 0;
    }
    return QpcToMicroseconds(end) - QpcToMicroseconds(start);
}

FrameSlot* ClaimFrameSlot(FrameRing* ring) noexcept
{
    for (UINT attempt = 0; attempt < UsbMonitorFrameRing::kSlotCount + 1; ++attempt)
    {
        for (UINT index = 0; index < UsbMonitorFrameRing::kSlotCount; ++index)
        {
            FrameSlot& slot = ring->slots[index];
            if (InterlockedCompareExchange(
                    reinterpret_cast<volatile LONG*>(&slot.state),
                    UsbMonitorFrameRing::kSlotWriting,
                    UsbMonitorFrameRing::kSlotFree) == UsbMonitorFrameRing::kSlotFree)
            {
                return &slot;
            }
        }

        UINT oldestIndex = UsbMonitorFrameRing::kSlotCount;
        std::uint64_t oldestSequence = (std::numeric_limits<std::uint64_t>::max)();
        for (UINT index = 0; index < UsbMonitorFrameRing::kSlotCount; ++index)
        {
            FrameSlot& slot = ring->slots[index];
            if (ReadInterlockedState(&slot.state) == UsbMonitorFrameRing::kSlotReady &&
                slot.sequence < oldestSequence)
            {
                oldestSequence = slot.sequence;
                oldestIndex = index;
            }
        }
        if (oldestIndex < UsbMonitorFrameRing::kSlotCount &&
            InterlockedCompareExchange(
                reinterpret_cast<volatile LONG*>(&ring->slots[oldestIndex].state),
                UsbMonitorFrameRing::kSlotWriting,
                UsbMonitorFrameRing::kSlotReady) == UsbMonitorFrameRing::kSlotReady)
        {
            return &ring->slots[oldestIndex];
        }
    }
    return nullptr;
}

void LoadBgraChannels16(
    const std::uint8_t* source,
    const __m128i& channelMask,
    const __m128i& zero,
    __m128i& blue,
    __m128i& green,
    __m128i& red) noexcept
{
    const __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source));
    blue = _mm_packs_epi32(_mm_and_si128(pixels, channelMask), zero);
    green = _mm_packs_epi32(
        _mm_and_si128(_mm_srli_epi32(pixels, 8), channelMask),
        zero);
    red = _mm_packs_epi32(
        _mm_and_si128(_mm_srli_epi32(pixels, 16), channelMask),
        zero);
}

void ConvertBgraToNv12(
    const D3D11_MAPPED_SUBRESOURCE& mapped,
    UINT width,
    UINT height,
    FrameSlot* slot) noexcept
{
    const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
    const std::size_t yPlaneBytes = static_cast<std::size_t>(width) * height;
    auto* yPlane = slot->payload;
    auto* uvPlane = slot->payload + yPlaneBytes;
    const __m128i zero = _mm_setzero_si128();
    const __m128i channelMask = _mm_set1_epi32(0xff);
    const __m128i yBlueCoefficient = _mm_set1_epi16(25);
    const __m128i yGreenCoefficient = _mm_set1_epi16(129);
    const __m128i yRedCoefficient = _mm_set1_epi16(66);
    const __m128i uvPairSums = _mm_set1_epi16(1);
    const __m128i uRedCoefficient = _mm_set1_epi16(-38);
    const __m128i uGreenCoefficient = _mm_set1_epi16(-74);
    const __m128i uBlueCoefficient = _mm_set1_epi16(112);
    const __m128i vRedCoefficient = _mm_set1_epi16(112);
    const __m128i vGreenCoefficient = _mm_set1_epi16(-94);
    const __m128i vBlueCoefficient = _mm_set1_epi16(-18);
    const __m128i bias128 = _mm_set1_epi16(128);

    for (UINT y = 0; y < height; ++y)
    {
        const auto* sourceRow = source + static_cast<std::size_t>(y) * mapped.RowPitch;
        auto* destinationRow = yPlane + static_cast<std::size_t>(y) * width;
        UINT x = 0;
        for (; x + 4 <= width; x += 4)
        {
            __m128i blue;
            __m128i green;
            __m128i red;
            LoadBgraChannels16(sourceRow + static_cast<std::size_t>(x) * 4,
                               channelMask,
                               zero,
                               blue,
                               green,
                               red);
            const __m128i blueProduct = _mm_unpacklo_epi16(
                _mm_mullo_epi16(blue, yBlueCoefficient),
                zero);
            const __m128i greenProduct = _mm_unpacklo_epi16(
                _mm_mullo_epi16(green, yGreenCoefficient),
                zero);
            const __m128i redProduct = _mm_unpacklo_epi16(
                _mm_mullo_epi16(red, yRedCoefficient),
                zero);
            __m128i yValues = _mm_add_epi32(
                _mm_add_epi32(blueProduct, greenProduct),
                redProduct);
            yValues = _mm_srli_epi32(_mm_add_epi32(yValues, _mm_set1_epi32(128)), 8);
            yValues = _mm_add_epi32(yValues, _mm_set1_epi32(16));
            const __m128i yBytes = _mm_packus_epi16(_mm_packs_epi32(yValues, zero), zero);
            const std::uint32_t packed = static_cast<std::uint32_t>(_mm_cvtsi128_si32(yBytes));
            std::memcpy(destinationRow + x, &packed, sizeof(packed));
        }
        for (; x < width; ++x)
        {
            const auto* pixel = sourceRow + static_cast<std::size_t>(x) * 4;
            const int blue = pixel[0];
            const int green = pixel[1];
            const int red = pixel[2];
            destinationRow[x] = ClampByte(((66 * red + 129 * green + 25 * blue + 128) >> 8) + 16);
        }
    }

    for (UINT y = 0; y < height; y += 2)
    {
        const auto* row0 = source + static_cast<std::size_t>(y) * mapped.RowPitch;
        const auto* row1 = source + static_cast<std::size_t>(y + 1) * mapped.RowPitch;
        auto* destinationRow = uvPlane + static_cast<std::size_t>(y / 2) * width;
        UINT x = 0;
        for (; x + 4 <= width; x += 4)
        {
            __m128i blue0;
            __m128i green0;
            __m128i red0;
            __m128i blue1;
            __m128i green1;
            __m128i red1;
            LoadBgraChannels16(row0 + static_cast<std::size_t>(x) * 4,
                               channelMask,
                               zero,
                               blue0,
                               green0,
                               red0);
            LoadBgraChannels16(row1 + static_cast<std::size_t>(x) * 4,
                               channelMask,
                               zero,
                               blue1,
                               green1,
                               red1);
            const __m128i blueSums = _mm_madd_epi16(
                _mm_add_epi16(blue0, blue1),
                uvPairSums);
            const __m128i greenSums = _mm_madd_epi16(
                _mm_add_epi16(green0, green1),
                uvPairSums);
            const __m128i redSums = _mm_madd_epi16(
                _mm_add_epi16(red0, red1),
                uvPairSums);
            const __m128i blueAverage = _mm_srli_epi16(
                _mm_packs_epi32(blueSums, zero),
                2);
            const __m128i greenAverage = _mm_srli_epi16(
                _mm_packs_epi32(greenSums, zero),
                2);
            const __m128i redAverage = _mm_srli_epi16(
                _mm_packs_epi32(redSums, zero),
                2);
            __m128i uValues = _mm_add_epi16(
                _mm_add_epi16(
                    _mm_mullo_epi16(redAverage, uRedCoefficient),
                    _mm_mullo_epi16(greenAverage, uGreenCoefficient)),
                _mm_mullo_epi16(blueAverage, uBlueCoefficient));
            uValues = _mm_add_epi16(_mm_srai_epi16(_mm_add_epi16(uValues, bias128), 8), bias128);
            __m128i vValues = _mm_add_epi16(
                _mm_add_epi16(
                    _mm_mullo_epi16(redAverage, vRedCoefficient),
                    _mm_mullo_epi16(greenAverage, vGreenCoefficient)),
                _mm_mullo_epi16(blueAverage, vBlueCoefficient));
            vValues = _mm_add_epi16(_mm_srai_epi16(_mm_add_epi16(vValues, bias128), 8), bias128);
            const __m128i uvBytes = _mm_packus_epi16(_mm_unpacklo_epi16(uValues, vValues), zero);
            const std::uint32_t packed = static_cast<std::uint32_t>(_mm_cvtsi128_si32(uvBytes));
            std::memcpy(destinationRow + x, &packed, sizeof(packed));
        }
        for (; x < width; x += 2)
        {
            int blue = 0;
            int green = 0;
            int red = 0;
            for (UINT dy = 0; dy < 2; ++dy)
            {
                const auto* row = dy == 0 ? row0 : row1;
                for (UINT dx = 0; dx < 2; ++dx)
                {
                    const auto* pixel = row + static_cast<std::size_t>(x + dx) * 4;
                    blue += pixel[0];
                    green += pixel[1];
                    red += pixel[2];
                }
            }
            blue /= 4;
            green /= 4;
            red /= 4;
            destinationRow[x] = ClampByte(((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
            destinationRow[x + 1] = ClampByte(((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
        }
    }
}

constexpr UINT kMonitorCount = 1;
constexpr UINT kModeCount = 3;

struct DisplayMode
{
    UINT Width;
    UINT Height;
    UINT RefreshRate;
};

constexpr DisplayMode kModes[kModeCount] =
{
    { 1920, 1080, 60 },
    { 1920, 1200, 60 },
    { 2560, 1600, 60 },
};

void FillSignalInfo(
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal,
    UINT width,
    UINT height,
    UINT refreshRate,
    bool monitorMode)
{
    signal.totalSize.cx = width;
    signal.totalSize.cy = height;
    signal.activeSize.cx = width;
    signal.activeSize.cy = height;
    signal.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
    signal.AdditionalSignalInfo.videoStandard = 255;
    signal.vSyncFreq.Numerator = refreshRate;
    signal.vSyncFreq.Denominator = 1;
    signal.hSyncFreq.Numerator = refreshRate * height;
    signal.hSyncFreq.Denominator = 1;
    signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    signal.pixelRate = static_cast<UINT64>(refreshRate) * width * height;
}

IDDCX_MONITOR_MODE MakeMonitorMode(const DisplayMode& mode)
{
    IDDCX_MONITOR_MODE result{};
    result.Size = sizeof(result);
    result.Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    FillSignalInfo(result.MonitorVideoSignalInfo, mode.Width, mode.Height, mode.RefreshRate, true);
    return result;
}

IDDCX_TARGET_MODE MakeTargetMode(const DisplayMode& mode)
{
    IDDCX_TARGET_MODE result{};
    result.Size = sizeof(result);
    FillSignalInfo(result.TargetVideoSignalInfo.targetVideoSignalInfo,
                   mode.Width,
                   mode.Height,
                   mode.RefreshRate,
                   false);
    return result;
}
}

extern "C" BOOL WINAPI DllMain(
    HINSTANCE instance,
    UINT reason,
    LPVOID reserved)
{
    UNREFERENCED_PARAMETER(instance);
    UNREFERENCED_PARAMETER(reason);
    UNREFERENCED_PARAMETER(reserved);
    return TRUE;
}

extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT driverObject,
    PUNICODE_STRING registryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, UsbMonitorDeviceAdd);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    return WdfDriverCreate(
        driverObject,
        registryPath,
        &attributes,
        &config,
        WDF_NO_HANDLE);
}

NTSTATUS UsbMonitorDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit)
{
    UNREFERENCED_PARAMETER(driver);

    WDF_PNPPOWER_EVENT_CALLBACKS powerCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&powerCallbacks);
    powerCallbacks.EvtDeviceD0Entry = UsbMonitorDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &powerCallbacks);

    IDD_CX_CLIENT_CONFIG iddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfig);
    iddConfig.EvtIddCxAdapterInitFinished = UsbMonitorAdapterInitFinished;
    iddConfig.EvtIddCxParseMonitorDescription = UsbMonitorParseMonitorDescription;
    iddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = UsbMonitorMonitorGetDefaultModes;
    iddConfig.EvtIddCxMonitorQueryTargetModes = UsbMonitorMonitorQueryTargetModes;
    iddConfig.EvtIddCxAdapterCommitModes = UsbMonitorAdapterCommitModes;
    iddConfig.EvtIddCxMonitorAssignSwapChain = UsbMonitorMonitorAssignSwapChain;
    iddConfig.EvtIddCxMonitorUnassignSwapChain = UsbMonitorMonitorUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &iddConfig);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContextWrapper);
    attributes.EvtCleanupCallback = [](WDFOBJECT object)
    {
        WdfObjectGet_DeviceContextWrapper(object)->Cleanup();
    };

    WDFDEVICE device{};
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    auto* wrapper = WdfObjectGet_DeviceContextWrapper(device);
    wrapper->Context = new (std::nothrow) IndirectDeviceContext(device);
    if (wrapper->Context == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

NTSTATUS UsbMonitorDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previousState)
{
    UNREFERENCED_PARAMETER(previousState);

    auto* wrapper = WdfObjectGet_DeviceContextWrapper(device);
    if (wrapper == nullptr || wrapper->Context == nullptr)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    return wrapper->Context->InitializeAdapter();
}

void DeviceContextWrapper::Cleanup()
{
    delete Context;
    Context = nullptr;
}

void MonitorContextWrapper::Cleanup()
{
    delete Context;
    Context = nullptr;
}

HRESULT Direct3DDevice::Initialize()
{
    HRESULT status = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(status))
    {
        return status;
    }

    status = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(status))
    {
        return status;
    }

    return D3D11CreateDevice(
        Adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &Device,
        nullptr,
        &DeviceContext);
}

SwapChainProcessor::SwapChainProcessor(
    IDDCX_SWAPCHAIN swapChain,
    std::shared_ptr<Direct3DDevice> device,
    HANDLE newFrameEvent)
    : m_SwapChain(swapChain),
      m_Device(std::move(device)),
      m_NewFrameEvent(newFrameEvent),
      m_TerminateEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr))
{
    if (m_TerminateEvent == nullptr)
    {
        m_StartStatus = HRESULT_FROM_WIN32(GetLastError());
        return;
    }
    m_Thread = CreateThread(nullptr, 0, RunThread, this, 0, nullptr);
    if (m_Thread == nullptr)
    {
        m_StartStatus = HRESULT_FROM_WIN32(GetLastError());
    }
}

// The host creates these named objects with an explicit ACL. Global\\ is required
// because the host is interactive while WUDFHost runs in session 0.
bool SwapChainProcessor::EnsureFrameRing()
{
    if (m_FrameRing != nullptr && m_FrameMapping != nullptr && m_FrameReadyEvent != nullptr)
    {
        if (ValidateFrameRing(m_FrameRing))
        {
            return true;
        }
        CloseFrameRing();
    }

    HANDLE mapping = OpenFileMappingW(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        UsbMonitorFrameRing::kMappingName);
    if (mapping == nullptr)
    {
        return false;
    }

    HANDLE readyEvent = OpenEventW(
        EVENT_MODIFY_STATE,
        FALSE,
        UsbMonitorFrameRing::kFrameReadyEventName);
    if (readyEvent == nullptr)
    {
        CloseHandle(mapping);
        return false;
    }

    void* view = MapViewOfFile(
        mapping,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(UsbMonitorFrameRing::FrameRing));
    if (view == nullptr)
    {
        CloseHandle(readyEvent);
        CloseHandle(mapping);
        return false;
    }

    auto* ring = static_cast<UsbMonitorFrameRing::FrameRing*>(view);
    if (!ValidateFrameRing(ring))
    {
        UnmapViewOfFile(view);
        CloseHandle(readyEvent);
        CloseHandle(mapping);
        return false;
    }

    m_FrameMapping = mapping;
    m_FrameReadyEvent = readyEvent;
    m_FrameRing = ring;
    return true;
}

void SwapChainProcessor::CloseFrameRing() noexcept
{
    if (m_FrameRing != nullptr)
    {
        UnmapViewOfFile(m_FrameRing);
        m_FrameRing = nullptr;
    }
    if (m_FrameReadyEvent != nullptr)
    {
        CloseHandle(m_FrameReadyEvent);
        m_FrameReadyEvent = nullptr;
    }
    if (m_FrameMapping != nullptr)
    {
        CloseHandle(m_FrameMapping);
        m_FrameMapping = nullptr;
    }
}

bool SwapChainProcessor::EnsureStagingTexture(UINT width, UINT height)
{
    if (m_StagingTexture != nullptr &&
        m_StagingWidth == width &&
        m_StagingHeight == height)
    {
        return true;
    }

    m_StagingTexture.Reset();
    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = width;
    stagingDesc.Height = height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    if (FAILED(m_Device->Device->CreateTexture2D(&stagingDesc, nullptr, &m_StagingTexture)))
    {
        m_StagingWidth = 0;
        m_StagingHeight = 0;
        return false;
    }
    m_StagingWidth = width;
    m_StagingHeight = height;
    return true;
}

bool SwapChainProcessor::CaptureAndPublish(
    IDXGIResource* surface,
    UINT64 presentDisplayQpcTime)
{
    if (surface == nullptr || !EnsureFrameRing())
    {
        return false;
    }

    ComPtr<ID3D11Texture2D> sourceTexture;
    if (FAILED(surface->QueryInterface(IID_PPV_ARGS(&sourceTexture))))
    {
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};
    sourceTexture->GetDesc(&sourceDesc);
    if (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
        sourceDesc.Width == 0 ||
        sourceDesc.Height == 0 ||
        sourceDesc.Width > UsbMonitorFrameRing::kMaxWidth ||
        sourceDesc.Height > UsbMonitorFrameRing::kMaxHeight ||
        (sourceDesc.Width & 1u) != 0 ||
        (sourceDesc.Height & 1u) != 0 ||
        sourceDesc.ArraySize != 1 ||
        sourceDesc.MipLevels != 1 ||
        sourceDesc.SampleDesc.Count != 1)
    {
        return false;
    }

    if (!EnsureStagingTexture(sourceDesc.Width, sourceDesc.Height))
    {
        return false;
    }
    const UINT64 copyStartQpc = QueryQpc();
    m_Device->DeviceContext->CopyResource(m_StagingTexture.Get(), sourceTexture.Get());
    const UINT64 copyEndQpc = QueryQpc();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const UINT64 mapStartQpc = QueryQpc();
    const HRESULT mapResult = m_Device->DeviceContext->Map(
        m_StagingTexture.Get(),
        0,
        D3D11_MAP_READ,
        0,
        &mapped);
    const UINT64 mapEndQpc = QueryQpc();
    if (FAILED(mapResult))
    {
        return false;
    }

    FrameSlot* slot = ClaimFrameSlot(m_FrameRing);
    if (slot == nullptr)
    {
        m_Device->DeviceContext->Unmap(m_StagingTexture.Get(), 0);
        return false;
    }

    const std::uint32_t width = sourceDesc.Width;
    const std::uint32_t height = sourceDesc.Height;
    const std::uint64_t captureTimestampUs = QpcToMicroseconds(presentDisplayQpcTime);
    std::uint64_t timestampUs = captureTimestampUs;
    if (timestampUs == 0)
    {
        LARGE_INTEGER nowQpc{};
        timestampUs = QueryPerformanceCounter(&nowQpc)
            ? QpcToMicroseconds(static_cast<UINT64>(nowQpc.QuadPart))
            : 0;
    }
    if (timestampUs == 0)
    {
        m_Device->DeviceContext->Unmap(m_StagingTexture.Get(), 0);
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(&slot->state),
            UsbMonitorFrameRing::kSlotFree);
        return false;
    }
    slot->sequence = m_NextSequence++;
    slot->frameId = m_NextFrameId++;
    slot->captureTimestampUs = timestampUs;
    slot->width = width;
    slot->height = height;
    slot->stride = width;
    slot->payloadLength = static_cast<std::uint32_t>(
        static_cast<std::size_t>(width) * height * 3 / 2);
    const UINT64 convertStartQpc = QueryQpc();
    ConvertBgraToNv12(mapped, width, height, slot);
    const UINT64 convertEndQpc = QueryQpc();
    m_Device->DeviceContext->Unmap(m_StagingTexture.Get(), 0);

    InterlockedExchangeAdd64(
        reinterpret_cast<volatile LONG64*>(&m_FrameRing->telemetry.copyTotalUs),
        static_cast<LONG64>(QpcDeltaToMicroseconds(copyStartQpc, copyEndQpc)));
    InterlockedExchangeAdd64(
        reinterpret_cast<volatile LONG64*>(&m_FrameRing->telemetry.mapTotalUs),
        static_cast<LONG64>(QpcDeltaToMicroseconds(mapStartQpc, mapEndQpc)));
    InterlockedExchangeAdd64(
        reinterpret_cast<volatile LONG64*>(&m_FrameRing->telemetry.convertTotalUs),
        static_cast<LONG64>(QpcDeltaToMicroseconds(convertStartQpc, convertEndQpc)));
    InterlockedIncrement64(
        reinterpret_cast<volatile LONG64*>(&m_FrameRing->telemetry.framesPublished));

    InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&slot->state),
        UsbMonitorFrameRing::kSlotReady);
    if (!SetEvent(m_FrameReadyEvent))
    {
        CloseFrameRing();
        return false;
    }
    return true;
}

SwapChainProcessor::~SwapChainProcessor()
{
    if (m_Thread != nullptr)
    {
        SetEvent(m_TerminateEvent);
        WaitForSingleObject(m_Thread, INFINITE);
        CloseHandle(m_Thread);
    }
    CloseFrameRing();
    if (m_TerminateEvent != nullptr)
    {
        CloseHandle(m_TerminateEvent);
    }
    if (m_SwapChain != nullptr)
    {
        WdfObjectDelete(static_cast<WDFOBJECT>(m_SwapChain));
        m_SwapChain = nullptr;
    }
}

DWORD WINAPI SwapChainProcessor::RunThread(LPVOID argument)
{
    static_cast<SwapChainProcessor*>(argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    DWORD taskIndex = 0;
    HANDLE avrtHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &taskIndex);
    RunCore();
    if (m_SwapChain != nullptr)
    {
        WdfObjectDelete(static_cast<WDFOBJECT>(m_SwapChain));
        m_SwapChain = nullptr;
    }
    if (avrtHandle != nullptr)
    {
        AvRevertMmThreadCharacteristics(avrtHandle);
    }
}

void SwapChainProcessor::RunCore()
{
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_Device->Device.As(&dxgiDevice)))
    {
        return;
    }

    IDARG_IN_SWAPCHAINSETDEVICE setDevice{};
    setDevice.pDevice = dxgiDevice.Get();
    if (FAILED(IddCxSwapChainSetDevice(m_SwapChain, &setDevice)))
    {
        return;
    }

    for (;;)
    {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer{};
        HRESULT status = IddCxSwapChainReleaseAndAcquireBuffer(m_SwapChain, &buffer);
        if (status == E_PENDING)
        {
            HANDLE waitHandles[] = { m_NewFrameEvent, m_TerminateEvent };
            DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, 16);
            if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT)
            {
                continue;
            }
            break;
        }
        if (FAILED(status))
        {
            break;
        }

        ComPtr<IDXGIResource> acquiredSurface;
        acquiredSurface.Attach(buffer.MetaData.pSurface);
        CaptureAndPublish(acquiredSurface.Get(), buffer.MetaData.PresentDisplayQPCTime);
        acquiredSurface.Reset();
        if (FAILED(IddCxSwapChainFinishedProcessingFrame(m_SwapChain)))
        {
            break;
        }
    }
    CloseFrameRing();
}

NTSTATUS IndirectDeviceContext::InitializeAdapter()
{
    if (m_Adapter != nullptr)
    {
        return STATUS_SUCCESS;
    }

    IDDCX_ADAPTER_CAPS adapterCaps{};
    adapterCaps.Size = sizeof(adapterCaps);
    adapterCaps.MaxMonitorsSupported = kMonitorCount;
    adapterCaps.EndPointDiagnostics.Size = sizeof(adapterCaps.EndPointDiagnostics);
    adapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    adapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    adapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"USB Monitor Transport";
    adapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"USB Monitor Transport";
    adapterCaps.EndPointDiagnostics.pEndPointModelName = L"Virtual Display";

    IDDCX_ENDPOINT_VERSION version{};
    version.Size = sizeof(version);
    version.MajorVer = 1;
    adapterCaps.EndPointDiagnostics.pFirmwareVersion = &version;
    adapterCaps.EndPointDiagnostics.pHardwareVersion = &version;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT init{};
    init.WdfDevice = m_Device;
    init.pCaps = &adapterCaps;
    init.ObjectAttributes = &attributes;

    IDARG_OUT_ADAPTER_INIT initOut{};
    NTSTATUS status = IddCxAdapterInitAsync(&init, &initOut);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    m_Adapter = initOut.AdapterObject;
    WdfObjectGet_DeviceContextWrapper(m_Adapter)->Context = this;
    return STATUS_SUCCESS;
}

NTSTATUS IndirectDeviceContext::CreateMonitor(UINT connectorIndex)
{
    if (m_Adapter == nullptr)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MonitorContextWrapper);
    attributes.EvtCleanupCallback = [](WDFOBJECT object)
    {
        WdfObjectGet_MonitorContextWrapper(object)->Cleanup();
    };
    IDDCX_MONITOR_INFO monitorInfo{};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    monitorInfo.ConnectorIndex = connectorIndex;
    monitorInfo.MonitorDescription.Size = sizeof(monitorInfo.MonitorDescription);
    monitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    monitorInfo.MonitorDescription.DataSize = 0;
    monitorInfo.MonitorDescription.pData = nullptr;
    monitorInfo.MonitorContainerId = {
        0x6e2d0a3c, 0x4c2e, 0x46e6,
        { 0x95, 0x9a, 0x6a, 0x5c, 0x0e, 0x14, 0x11, 0x22 }
    };

    IDARG_IN_MONITORCREATE create{};
    create.ObjectAttributes = &attributes;
    create.pMonitorInfo = &monitorInfo;

    IDARG_OUT_MONITORCREATE createOut{};
    NTSTATUS status = IddCxMonitorCreate(m_Adapter, &create, &createOut);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    auto* context = new (std::nothrow) IndirectMonitorContext(createOut.MonitorObject);
    if (context == nullptr)
    {
        WdfObjectDelete(static_cast<WDFOBJECT>(createOut.MonitorObject));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    WdfObjectGet_MonitorContextWrapper(createOut.MonitorObject)->Context = context;

    IDARG_OUT_MONITORARRIVAL arrival{};
    status = IddCxMonitorArrival(createOut.MonitorObject, &arrival);
    if (!NT_SUCCESS(status))
    {
        WdfObjectDelete(static_cast<WDFOBJECT>(createOut.MonitorObject));
        return status;
    }
    return STATUS_SUCCESS;
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    m_Processor.reset();
}

NTSTATUS IndirectMonitorContext::AssignSwapChain(
    IDDCX_SWAPCHAIN swapChain,
    LUID renderAdapter,
    HANDLE newFrameEvent)
{
    m_Processor.reset();

    auto device = std::make_shared<Direct3DDevice>(renderAdapter);
    if (FAILED(device->Initialize()))
    {
        WdfObjectDelete(static_cast<WDFOBJECT>(swapChain));
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }

    auto processor = std::unique_ptr<SwapChainProcessor>(
        new (std::nothrow) SwapChainProcessor(swapChain, std::move(device), newFrameEvent));
    if (!processor)
    {
        WdfObjectDelete(static_cast<WDFOBJECT>(swapChain));
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }
    if (!processor->IsReady())
    {
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }
    m_Processor = std::move(processor);
    return STATUS_SUCCESS;
}

void IndirectMonitorContext::UnassignSwapChain()
{
    m_Processor.reset();
}

NTSTATUS UsbMonitorAdapterInitFinished(
    IDDCX_ADAPTER adapter,
    const IDARG_IN_ADAPTER_INIT_FINISHED* args)
{
    if (!NT_SUCCESS(args->AdapterInitStatus))
    {
        return args->AdapterInitStatus;
    }

    auto* wrapper = WdfObjectGet_DeviceContextWrapper(adapter);
    if (wrapper == nullptr || wrapper->Context == nullptr)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    return wrapper->Context->CreateMonitor(0);
}

NTSTATUS UsbMonitorAdapterCommitModes(
    IDDCX_ADAPTER adapter,
    const IDARG_IN_COMMITMODES* args)
{
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(args);
    return STATUS_SUCCESS;
}

NTSTATUS UsbMonitorParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* args,
    IDARG_OUT_PARSEMONITORDESCRIPTION* result)
{
    UNREFERENCED_PARAMETER(args);
    result->MonitorModeBufferOutputCount = 0;
    return STATUS_SUCCESS;
}

NTSTATUS UsbMonitorMonitorGetDefaultModes(
    IDDCX_MONITOR monitor,
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* args,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* result)
{
    UNREFERENCED_PARAMETER(monitor);
    result->DefaultMonitorModeBufferOutputCount = kModeCount;
    if (args->DefaultMonitorModeBufferInputCount == 0 || args->pDefaultMonitorModes == nullptr)
    {
        return STATUS_SUCCESS;
    }
    if (args->DefaultMonitorModeBufferInputCount < kModeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (UINT i = 0; i < kModeCount; ++i)
    {
        args->pDefaultMonitorModes[i] = MakeMonitorMode(kModes[i]);
    }
    result->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

NTSTATUS UsbMonitorMonitorQueryTargetModes(
    IDDCX_MONITOR monitor,
    const IDARG_IN_QUERYTARGETMODES* args,
    IDARG_OUT_QUERYTARGETMODES* result)
{
    UNREFERENCED_PARAMETER(monitor);
    result->TargetModeBufferOutputCount = kModeCount;
    if (args->TargetModeBufferInputCount == 0 || args->pTargetModes == nullptr)
    {
        return STATUS_SUCCESS;
    }
    if (args->TargetModeBufferInputCount < kModeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (UINT i = 0; i < kModeCount; ++i)
    {
        args->pTargetModes[i] = MakeTargetMode(kModes[i]);
    }
    return STATUS_SUCCESS;
}

NTSTATUS UsbMonitorMonitorAssignSwapChain(
    IDDCX_MONITOR monitor,
    const IDARG_IN_SETSWAPCHAIN* args)
{
    auto* wrapper = WdfObjectGet_MonitorContextWrapper(monitor);
    if (wrapper == nullptr || wrapper->Context == nullptr)
    {
        WdfObjectDelete(static_cast<WDFOBJECT>(args->hSwapChain));
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }
    return wrapper->Context->AssignSwapChain(
        args->hSwapChain,
        args->RenderAdapterLuid,
        args->hNextSurfaceAvailable);
}

NTSTATUS UsbMonitorMonitorUnassignSwapChain(IDDCX_MONITOR monitor)
{
    auto* wrapper = WdfObjectGet_MonitorContextWrapper(monitor);
    if (wrapper != nullptr && wrapper->Context != nullptr)
    {
        wrapper->Context->UnassignSwapChain();
    }
    return STATUS_SUCCESS;
}
