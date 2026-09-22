# Interaction Latency Test Plan

## Baseline

- Resolution: `1920x1200`
- Bitrate: `8000000`
- Queue capacity: `2`
- Transport: `adb reverse`
- Test date: September 22, 2026

Run both `30 FPS` and `60 FPS` at the same resolution and bitrate.

## Start

Stop any previous host process first. Use the Phase 2 host binary:

```powershell
adb reverse tcp:5000 tcp:5000
adb install -r .\apps\android\app\build\outputs\apk\debug\app-debug.apk
adb logcat -c
```

Terminal 1:

```powershell
.\build\host\phase2\second-screen-host.exe --width 1920 --height 1200 --fps 60 --bitrate 8000000 2>&1 | Tee-Object .\latency-host.log
```

Terminal 2:

```powershell
adb logcat -v time -s SecondScreenStream:I SecondScreenConnection:W 2>&1 | Tee-Object .\latency-android.log
```

## Camera Test

Record the Windows source and Android tablet in one `120 FPS` or `240 FPS` camera shot. For each action, capture at least 30 samples:

- continuous mouse motion
- window dragging
- typing into a visible text field
- scrolling a visible page

Repeat at `30 FPS`, then at `60 FPS`. Measure camera-frame count from the physical action to the matching tablet update.

## Report

| Config | Action | Samples | P50 ms | P95 ms | Notes |
| --- | --- | ---: | ---: | ---: | --- |
| 1920x1200 / 30 FPS | mouse | 30 |  |  |  |
| 1920x1200 / 60 FPS | mouse | 30 |  |  |  |

Add rows for dragging, typing, and scrolling.

## Internal Logs

Host `[STATS]` fields:

- `capture_fps`, `video_fps`: capture and packet production rate
- `encode_call_avg_ms`, `encode_call_max_ms`: encode call including its send call
- `send_avg_ms`, `send_max_ms`: socket write duration
- `skipped_delta`, `send_fail`, `send_timeout`: host-side loss indicators

Android `[STATS]` fields:

- `recv_fps`, `submit_fps`, `decode_fps`: receive, decoder submission, and output rates
- `queue_avg_ms`, `queue_max_ms`: packet receive to `MediaCodec` submission delay
- `drop_delta`: queue drops during the five-second window
- `input_starvation`, `reconnect`: decoder and transport failures

Do not subtract host and Android monotonic timestamps. Use the camera result for end-to-end latency.
