# Interaction Latency Specification

## Objective

Reduce the perceived delay between a Windows mouse or keyboard action and the matching visual update on the Android display without regressing stream stability.

This milestone measures visual feedback latency. Android touch-to-Windows input injection is not part of this milestone.

## Baseline

- Resolution: 1920x1200
- Refresh target: 60 FPS
- Bitrate: 8 Mbps
- Transport: TCP through `adb reverse`
- Encoder: NVIDIA NVENC with software fallback
- Decoder: Android `MediaCodec`
- Stability requirement: at least 30 minutes without freeze, reconnect, or progressive latency growth

## Required Telemetry

Every video frame keeps the existing `frameId` and capture timestamp. Add low-rate counters rather than per-frame logs.

Host report every five seconds:

- FrameRing frames received
- raw frames skipped before encoding
- encoded access units produced
- IDR access units produced
- bytes sent
- heartbeats sent
- send failures and send timeout count

Android report every five seconds:

- video packets received
- IDR packets received
- encoded frames dropped by the queue
- frames submitted to `MediaCodec`
- decoder input starvation count
- reconnect count and last reconnect reason

`MediaCodec.OnFrameRenderedListener` may be retained only as best-effort telemetry. It must never control connection health.

## Queue Semantics

H.264 access units are not independent.

- The queue may discard stale P-frames to bound latency.
- A pending IDR must not be replaced by a dependent P-frame.
- A frame-ID gap puts the decoder into random-access recovery.
- Recovery must complete at the next IDR.
- NVENC must emit IDR plus SPS/PPS at least four times per second.

## Latency Measurement

Use two complementary measurements:

1. Internal stage timing for capture, encode, send, receive, decode submission, and queue depth.
2. High-frame-rate camera measurement for true mouse/keyboard-to-photon latency.

Do not subtract Windows and Android monotonic timestamps directly until clock offset and drift are calibrated.

## Acceptance Criteria

- 30-minute continuous run without freeze or reconnect.
- No progressive increase in decoder queue depth.
- Decoder recovery after an intentional frame gap within 250 ms.
- P95 visual interaction latency below 80 ms.
- Stretch target: P95 below 50 ms.
- No keyboard event or click requires a stream reconnect to become visible.

## Optional Cursor Fast Path

Implement only if telemetry shows the video pipeline meets its target but the Windows cursor still feels delayed.

- Send cursor position and visibility as small control messages.
- Render the cursor as an Android overlay.
- Coalesce stale cursor-move messages; never coalesce button transitions.
- Keep this path independent from future Android touch injection.

## Non-Goals

- Native USB transport
- Android touch, stylus, or multitouch injection
- Audio
- HDR
- 120 Hz
- Wi-Fi transport
