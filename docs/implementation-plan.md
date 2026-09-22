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

## Commit Sequence

1. `Add stream telemetry summaries`
2. `Document interaction latency baseline`
3. `Optimize measured latency bottleneck`
4. `Add developer launch script`

Every commit must build the host and Android app independently.
