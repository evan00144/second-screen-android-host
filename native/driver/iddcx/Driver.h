#pragma once

#define NOMINMAX
#include <windows.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>
#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include "../../shared/FrameRing.h"

#include <memory>

namespace UsbMonitorIddCx
{
using Microsoft::WRL::ComPtr;

struct Direct3DDevice
{
    explicit Direct3DDevice(LUID adapterLuid) : AdapterLuid(adapterLuid) {}
    HRESULT Initialize();

    LUID AdapterLuid{};
    ComPtr<IDXGIFactory5> DxgiFactory;
    ComPtr<IDXGIAdapter1> Adapter;
    ComPtr<ID3D11Device> Device;
    ComPtr<ID3D11DeviceContext> DeviceContext;
};

struct PendingStagingFrame
{
    UINT stagingIndex{};
    UINT width{};
    UINT height{};
    UINT64 presentDisplayQpcTime{};
    std::uint64_t copyDurationUs{};
};

class SwapChainProcessor
{
public:
    SwapChainProcessor(IDDCX_MONITOR monitor,
                       IDDCX_SWAPCHAIN swapChain,
                       std::shared_ptr<Direct3DDevice> device,
                       HANDLE newFrameEvent);
    ~SwapChainProcessor();

    bool IsReady() const noexcept { return SUCCEEDED(m_StartStatus); }

private:
    static DWORD WINAPI RunThread(_In_ LPVOID argument);
    void Run();
    void RunCore();
    bool SetupHardwareCursor() noexcept;
    void QueryHardwareCursor() noexcept;
    bool EnsureFrameRing();
    void CloseFrameRing() noexcept;
    bool EnsureStagingTextures(UINT width, UINT height);
    bool MapAndPublishStagingFrame(const PendingStagingFrame& frame);
    bool CaptureAndPublish(
        ComPtr<IDXGIResource>& surface,
        UINT64 presentDisplayQpcTime);

    IDDCX_MONITOR m_Monitor{};
    IDDCX_SWAPCHAIN m_SwapChain{};
    std::shared_ptr<Direct3DDevice> m_Device;
    HANDLE m_NewFrameEvent{};
    HANDLE m_NewCursorEvent{};
    HANDLE m_Thread{};
    HANDLE m_TerminateEvent{};
    HANDLE m_FrameMapping{};
    HANDLE m_FrameReadyEvent{};
    UsbMonitorFrameRing::FrameRing* m_FrameRing{};
    ComPtr<ID3D11Texture2D> m_StagingTextures[2];
    UINT m_StagingWidth{};
    UINT m_StagingHeight{};
    UINT m_StagingWriteIndex{};
    PendingStagingFrame m_PendingStagingFrame{};
    bool m_HasPendingStagingFrame{};
    bool m_HasPublishedFrame{};
    std::unique_ptr<std::uint8_t[]> m_CursorShapeBuffer;
    DWORD m_LastCursorShapeId{};
    std::uint64_t m_NextFrameId{1};
    std::uint64_t m_NextSequence{1};
    HRESULT m_StartStatus{S_OK};
 };

class IndirectDeviceContext;
class IndirectMonitorContext;

struct DeviceContextWrapper
{
    IndirectDeviceContext* Context{};
    void Cleanup();
};

struct MonitorContextWrapper
{
    IndirectMonitorContext* Context{};
    void Cleanup();
};

WDF_DECLARE_CONTEXT_TYPE(DeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(MonitorContextWrapper);

class IndirectDeviceContext
{
public:
    explicit IndirectDeviceContext(WDFDEVICE device) : m_Device(device) {}
    ~IndirectDeviceContext() = default;

    NTSTATUS InitializeAdapter();
    NTSTATUS CreateMonitor(UINT connectorIndex);

private:
    WDFDEVICE m_Device{};
    IDDCX_ADAPTER m_Adapter{};
};

class IndirectMonitorContext
{
public:
    explicit IndirectMonitorContext(IDDCX_MONITOR monitor) : m_Monitor(monitor) {}
    ~IndirectMonitorContext();

    NTSTATUS AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
    void UnassignSwapChain();

private:
    IDDCX_MONITOR m_Monitor{};
    std::unique_ptr<SwapChainProcessor> m_Processor;
};

} // namespace UsbMonitorIddCx

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD UsbMonitorDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY UsbMonitorDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED UsbMonitorAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES UsbMonitorAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION UsbMonitorParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES UsbMonitorMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES UsbMonitorMonitorQueryTargetModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN UsbMonitorMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN UsbMonitorMonitorUnassignSwapChain;
