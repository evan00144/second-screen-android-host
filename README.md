# Second Screen Android Host

Windows indirect display streamed to an Android tablet over USB/ADB using H.264.

## Status

Phase 16 release-hardening build dated September 25, 2026:

- 1920x1200 at 60 FPS
- 8 Mbps H.264
- NVIDIA NVENC on RTX 2050
- Xiaomi 25099RP13G Android client
- Multi-minute streaming without freeze or reconnect
- Host/driver/Android release metadata: `0.16.0`

The current transport uses `adb reverse`. Native USB transport and Android touch injection remain future work.

## Components

- `native/driver/iddcx`: Windows IddCx virtual display driver and FrameRing producer.
- `native/streamer`: FrameRing reader, H.264 encoder, and TCP host.
- `apps/android`: Android TCP receiver and `MediaCodec` renderer.
- `scripts`: Driver installation helpers.
- `prd.md`: Product vision and original architecture.
- `docs/interaction-latency-spec.md`: Next latency objective and acceptance criteria.
- `docs/implementation-plan.md`: Ordered implementation plan.
- `docs/latency-test-plan.md`: Repeatable 30/60 FPS latency test procedure.

## Requirements

- Windows 10 or Windows 11
- Administrator PowerShell for driver and host execution
- Visual Studio 2022, Windows SDK, and WDK
- CMake 3.20 or newer
- Android SDK Platform Tools (`adb`)
- JDK 17 or newer and Gradle 8.9

## Build

Host:

```powershell
cmake -S .\native\streamer -B .\build\host -A x64
cmake --build .\build\host --config Release
```

Android:

```powershell
gradle -p .\apps\android :app:assembleDebug
```

Driver installation details remain in `scripts/install-driver.ps1` and `scripts/install-driver-one-boot.ps1`.

## Phase 16 Workflow

Build the Windows side only (host + virtual-display driver):

```powershell
.\scripts\phase16-release.ps1
```

Install the Windows driver, then verify a running host and Android client:

```powershell
.\scripts\phase16-release.ps1 -InstallDriver -VerifyRuntime
```

If Secure Boot blocks `testsigning`, boot Advanced startup -> Startup Settings -> Disable driver signature enforcement, then run:

```powershell
.\scripts\phase16-release.ps1 -Target Windows -InstallDriver -OneBoot
```

Build or install the Android APK separately:

```powershell
.\scripts\phase16-release.ps1 -Target Android
.\scripts\phase16-release.ps1 -Target Android -InstallAndroid
```

Build both artifacts without installing either:

```powershell
.\scripts\phase16-release.ps1 -Target All
```

Run checks without rebuilding:

```powershell
.\scripts\phase16-check.ps1 -RequireHost -RequireAndroid
```

Create a shareable diagnostic bundle:

```powershell
.\scripts\phase16-diagnostics.ps1
```

Windows and Android are separate install targets. The release flow does not stop an existing host. Stop the old host first when replacing binaries; the host now rejects concurrent FrameRing owners.

## Run

Install the Android app and configure ADB forwarding:

```powershell
adb install -r .\apps\android\app\build\outputs\apk\debug\app-debug.apk
adb reverse tcp:5000 tcp:5000
```

From an Administrator PowerShell:

```powershell
.\build\host\second-screen-host.exe --width 1920 --height 1200 --fps 60 --bitrate 8000000
```

These IPC messages are expected when the driver still owns the shared objects from a previous host run:

```text
[IPC] reusing existing FrameRing mapping; resetting shared state
[IPC] reusing existing FrameReady event; cleared stale signal
```

Restart the PnP device only when the driver itself must be reset; it is no longer required for normal host restarts.

## Current Reliability Rules

- Host sends heartbeat packets while the captured image is unchanged.
- Host sends cursor position sideband packets; Android renders a low-latency cursor overlay.
- Android does not treat a static desktop as a dead connection.
- Encoded-frame dropping preserves IDR frames.
- NVENC emits IDR plus SPS/PPS every quarter second.
- Socket writes fail within two seconds when the client stops reading.

## Next Work

Do not change the stable video pipeline without measurements. Cursor sideband is separate from H.264; tablet-to-Windows input remains out of scope. See `docs/implementation-plan.md` for the current phase.
