# Implementation Plan

## Phase 0: Stable Baseline

Status: complete.

- Shared FrameRing can be reused across host restarts.
- Static scenes use heartbeat packets instead of reconnect loops.
- Android watchdog monitors transport and decoder activity only.
- IDR frames survive latest-frame queue pressure.
- NVENC forces periodic IDR plus SPS/PPS.

Exit condition: verified multi-minute run without freeze or reconnect.

## Phase 1: Low-Rate Telemetry

Add one five-second summary log on each side.

Host files:

- `native/streamer/main.cpp`

Android files:

- `StreamStats.java`
- `StreamReceiver.java`
- `VideoDecoder.java`

Deliverables:

- Capture, encode, packet, IDR, byte, heartbeat, queue-drop, and decoder counters.
- One reason string for every reconnect.
- No per-frame logging.

Exit condition: a freeze or latency spike can be assigned to capture, encode, transport, queue, or decode from one host log and one `adb logcat` capture.

## Phase 2: Measure Interaction Latency

Status: instrumentation complete; camera measurements pending.

- Record a 60 FPS and, when available, 120/240 FPS camera test.
- Test mouse motion, window dragging, typing, and scrolling.
- Record P50 and P95 results for 30 FPS and 60 FPS configurations.
- Keep 1920x1200 at 8 Mbps as the comparison baseline.
- Use `docs/latency-test-plan.md` for the repeatable procedure.

Exit condition: one reproducible test table identifies the dominant stage.

## Phase 3: Targeted Optimization

Apply only the smallest change supported by Phase 2 data.

Candidate order:

1. Tune Android queue and decoder submission.
2. Tune NVENC rate control and keyframe cadence.
3. Reduce capture-copy cost.
4. Add cursor fast path only when cursor latency remains isolated.

Each change must pass the 30-minute stability test before the next change starts.

## Phase 4: Developer Packaging

Add one PowerShell entry point that:

- verifies Administrator privileges
- installs or validates the driver
- runs `adb reverse`
- optionally installs the latest debug APK
- starts the host with the validated baseline settings

Do not add a desktop GUI yet.

## Phase 5: V0.2 Features

Start only after latency and packaging are stable:

- Android touch-to-Windows input
- native USB transport
- automatic device discovery

These remain separate from the current mouse/keyboard visual-latency milestone.

## Phase 6: Capture Pipeline Overlap

Status: implementation complete; runtime validation pending.

- Use two D3D11 staging textures so GPU copy for the next frame overlaps CPU map and BGRA-to-NV12 conversion for the previous frame.
- Publish the first frame synchronously so a static display never starts black.
- Keep the existing FrameRing format and telemetry unchanged.

Exit condition: 30-second run at 1920x1200 and 60 FPS reaches at least 55 FPS without increasing queue drops, send timeouts, or reconnects.

## Phase 6.1: Early Swap-Chain Release

Status: implementation complete; runtime validation pending.

- Call IddCxSwapChainFinishedProcessingFrame immediately after CopyResource queues the GPU readback.
- Keep CPU Map and BGRA-to-NV12 conversion outside the swap-chain hold time.

Exit condition: capture cadence reaches at least 55 FPS at 1920x1200 and 60 Hz without regressions.

## Phase 6.2: Release Acquired Surface Early

Status: implementation complete; runtime validation pending.

- Release the acquired IDXGIResource immediately after CopyResource completes.
- Keep staging Map and CPU conversion outside both the acquired-surface lifetime and swap-chain hold time.

Exit condition: capture cadence improves beyond the 42–50 FPS baseline without regressions.

## Phase 6.3: AVX2 BGRA-to-NV12 Conversion

Status: implementation complete; runtime validation pending.

- Compile the hot conversion loop with `/arch:AVX2` in a separate translation unit.
- Dispatch at runtime through CPUID, retaining the existing SSE2 converter as fallback.
- Vectorize eight-pixel luma and four-sample 2x2 chroma batches; preserve the existing NV12 layout and FrameRing telemetry.

Exit condition: conversion time drops materially from the 4 ms baseline on AVX2 hardware without changing frame bytes or increasing queue drops.

## Phase 6.4: NVENC Stage Telemetry

Status: implementation complete; runtime validation pending.

- Split synchronous NVENC timing into input lock, NV12 copy, encode submission, and output lock.
- Keep existing aggregate encode and socket-send metrics for comparison.
- Use the stage breakdown to choose async NVENC or buffer-pool work without guessing.

Exit condition: one five-second host summary identifies the dominant encode-stage stall.

## Phase 6.5: Pipelined NVENC Output

Status: implementation complete; runtime validation pending.

- Use three reusable NVENC input and bitstream slots.
- Use NVENC asynchronous completion events per slot; do not lock an output before its event fires.
- Poll the oldest output in order; block only when all slots are occupied.
- Preserve frame metadata, IDR cadence, packet ordering, and software fallback.

Exit condition: 30-second 1920x1200 run reaches at least 55 video FPS with output wait materially below the 12 ms baseline, no send timeouts, and no sustained queue drops.

## Phase 7: Cursor Sideband

Status: implementation complete; runtime validation pending.

- Poll the Windows cursor on the host at low latency and coalesce unchanged positions.
- Normalize monitor coordinates to the encoded stream dimensions before sending them.
- Send cursor visibility and coordinates as small SSV1 control packets outside the H.264 queue.
- Render a lightweight cursor overlay above the Android `SurfaceView`, matching its X/Y presentation scale.
- Report cursor packet/update deltas and expose current-window FPS/bitrate in the Android overlay.
- Keep tablet-to-Windows input injection out of scope.

Exit condition: cursor movement remains responsive while video keeps its existing frame/drop/reconnect behavior.

## Phase 10: Pipelined D3D11 Readback

Status: implementation complete; runtime validation pending.

- Queue the next GPU staging copy before mapping and converting the previous frame.
- Keep the first published frame synchronous so a static display does not start black.
- Flush one pending staging frame during swap-chain shutdown or resolution changes.
- Preserve the existing FrameRing format and driver telemetry.

Exit condition: 30-second 1920x1200 run at 60 Hz reaches at least 55 capture FPS without increasing queue drops, send timeouts, or reconnects.

## Phase 11: Idle Frame Refresh

Status: implementation complete; runtime validation pending.

- Re-encode the latest captured frame at 30 FPS after one refresh interval without a new FrameRing frame.
- Keep idle-refresh deadlines cadence-anchored, skipping missed deadlines instead of accumulating timer drift.
- Keep captured-frame cadence telemetry separate from idle refresh telemetry.
- Preserve heartbeats for connection watchdog behavior.

Exit condition: idle desktop text input remains responsive without increasing reconnects, send timeouts, or decoder starvation.

## Phase 14: Event-Driven Swap-Chain Wait

Status: implementation complete; runtime validation pending.

- Wait directly on IddCx new-frame, cursor, and termination events instead of polling every 4 ms.
- Keep fresh-frame cadence telemetry separate from idle refresh cadence.
- Preserve the existing FrameRing payload and host-side idle refresh fallback.

Exit condition: active capture keeps its existing frame/drop/reconnect behavior with lower driver wakeup overhead.

## Phase 15: Asynchronous CPU Conversion

Status: implementation complete; runtime validation pending.

- Keep D3D11 staging readback on the swap-chain thread, but copy mapped BGRA rows into a bounded three-buffer queue.
- Convert BGRA to NV12 and publish FrameRing slots on a worker thread.
- Keep the first frame synchronous, preserve frame order, and drain queued frames during shutdown or resolution changes.
- Preserve existing copy, map, convert, drop, and reconnect telemetry.

Exit condition: active capture improves toward 55 FPS with no increase in queue drops, send timeouts, or reconnects.

## Commit Sequence

1. `Add stream telemetry summaries`
2. `Document interaction latency baseline`
3. `Optimize measured latency bottleneck`
4. `Add developer launch script`

Every commit must build the host and Android app independently.
