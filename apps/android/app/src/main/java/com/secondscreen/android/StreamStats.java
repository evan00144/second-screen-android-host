package com.secondscreen.android;

import java.util.Locale;
import java.util.concurrent.atomic.AtomicLong;

final class StreamStats {
    private final AtomicLong videoPacketsReceived = new AtomicLong();
    private final AtomicLong idrPacketsReceived = new AtomicLong();
    private final AtomicLong decodedFrames = new AtomicLong();
    private final AtomicLong queueDrops = new AtomicLong();
    private final AtomicLong framesSubmitted = new AtomicLong();
    private final AtomicLong decoderInputStarvation = new AtomicLong();
    private final AtomicLong heartbeatPackets = new AtomicLong();
    private final AtomicLong cursorUpdates = new AtomicLong();
    private final AtomicLong receivedBytes = new AtomicLong();
    private final AtomicLong queueDelaySamples = new AtomicLong();
    private final AtomicLong queueDelayNanosTotal = new AtomicLong();
    private final AtomicLong queueDelayNanosMax = new AtomicLong();
    private final AtomicLong reconnectCount = new AtomicLong();
    private volatile long lastCaptureTimestampUs;
    private volatile long lastPacketReceivedNanos;
    private volatile long lastFrameReceivedNanos;
    private volatile long lastDecoderActivityNanos;
    private volatile long startedNanos = System.nanoTime();
    private volatile int width;
    private volatile int height;
    private volatile String lastReconnectReason = "none";
    private long lastSnapshotNanos;
    private long lastSnapshotDecoded;
    private long lastSnapshotBytes;
    private double windowFps;
    private double windowBitrate;

    void beginStream(int width, int height) {
        this.width = width;
        this.height = height;
        videoPacketsReceived.set(0);
        idrPacketsReceived.set(0);
        decodedFrames.set(0);
        queueDrops.set(0);
        framesSubmitted.set(0);
        decoderInputStarvation.set(0);
        heartbeatPackets.set(0);
        cursorUpdates.set(0);
        receivedBytes.set(0);
        queueDelaySamples.set(0);
        queueDelayNanosTotal.set(0);
        queueDelayNanosMax.set(0);
        lastCaptureTimestampUs = 0;
        lastPacketReceivedNanos = 0;
        lastFrameReceivedNanos = 0;
        lastDecoderActivityNanos = 0;
        startedNanos = System.nanoTime();
        lastSnapshotNanos = startedNanos;
        lastSnapshotDecoded = 0;
        lastSnapshotBytes = 0;
        windowFps = 0.0;
        windowBitrate = 0.0;
    }

    void packetReceived(int bytes, long captureTimestampUs, boolean randomAccess) {
        videoPacketsReceived.incrementAndGet();
        if (randomAccess) {
            idrPacketsReceived.incrementAndGet();
        }
        receivedBytes.addAndGet(bytes);
        lastCaptureTimestampUs = captureTimestampUs;
        long now = System.nanoTime();
        lastPacketReceivedNanos = now;
        lastFrameReceivedNanos = now;
    }

    void heartbeatReceived() {
        heartbeatPackets.incrementAndGet();
        lastPacketReceivedNanos = System.nanoTime();
    }

    void cursorUpdateReceived() {
        cursorUpdates.incrementAndGet();
    }

    void frameDecoded() {
        decodedFrames.incrementAndGet();
    }

    void decoderActivity() {
        lastDecoderActivityNanos = System.nanoTime();
    }

    void frameSubmitted(long queueDelayNanos) {
        framesSubmitted.incrementAndGet();
        if (queueDelayNanos >= 0) {
            queueDelaySamples.incrementAndGet();
            queueDelayNanosTotal.addAndGet(queueDelayNanos);
            queueDelayNanosMax.accumulateAndGet(queueDelayNanos, Math::max);
        }
    }

    void decoderInputStarved() {
        decoderInputStarvation.incrementAndGet();
    }

    void framesDropped(long count) {
        if (count > 0) {
            queueDrops.addAndGet(count);
        }
    }

    void reconnect(String reason) {
        reconnectCount.incrementAndGet();
        String normalized = reason == null ? "unknown" : reason.replace('\n', ' ').replace('\r', ' ');
        lastReconnectReason = normalized.isEmpty() ? "unknown" : normalized;
    }

    synchronized Snapshot snapshot() {
        long nowNanos = System.nanoTime();
        long elapsedNanos = Math.max(1L, nowNanos - startedNanos);
        double seconds = elapsedNanos / 1_000_000_000.0;
        long decoded = decodedFrames.get();
        long bytes = receivedBytes.get();
        double fps = decoded / seconds;
        double bitrate = bytes * 8.0 / seconds;
        long snapshotElapsedNanos = Math.max(1L, nowNanos - lastSnapshotNanos);
        double snapshotSeconds = snapshotElapsedNanos / 1_000_000_000.0;
        windowFps = (decoded - lastSnapshotDecoded) / snapshotSeconds;
        windowBitrate = (bytes - lastSnapshotBytes) * 8.0 / snapshotSeconds;
        lastSnapshotNanos = nowNanos;
        lastSnapshotDecoded = decoded;
        lastSnapshotBytes = bytes;

        long latencyMs = -1L;
        return new Snapshot(
                width,
                height,
                fps,
                bitrate,
                windowFps,
                windowBitrate,
                latencyMs,
                videoPacketsReceived.get(),
                idrPacketsReceived.get(),
                decodedFrames.get(),
                queueDrops.get(),
                framesSubmitted.get(),
                decoderInputStarvation.get(),
                heartbeatPackets.get(),
                cursorUpdates.get(),
                reconnectCount.get(),
                lastReconnectReason,
                queueDelaySamples.get(),
                queueDelayNanosTotal.get(),
                queueDelayNanosMax.get(),
                lastPacketReceivedNanos,
                lastFrameReceivedNanos,
                lastDecoderActivityNanos);
    }

    static final class Snapshot {
        final int width;
        final int height;
        final double fps;
        final double bitrate;
        final double windowFps;
        final double windowBitrate;
        final long latencyMs;
        final long videoPacketsReceived;
        final long idrPacketsReceived;
        final long decodedFrames;
        final long droppedFrames;
        final long framesSubmitted;
        final long decoderInputStarvation;
        final long heartbeatPackets;
        final long cursorUpdates;
        final long reconnectCount;
        final String lastReconnectReason;
        final long queueDelaySamples;
        final long queueDelayNanosTotal;
        final long queueDelayNanosMax;
        final long lastPacketReceivedNanos;
        final long lastFrameReceivedNanos;
        final long lastDecoderActivityNanos;

        Snapshot(
                int width,
                int height,
                double fps,
                double bitrate,
                double windowFps,
                double windowBitrate,
                long latencyMs,
                long videoPacketsReceived,
                long idrPacketsReceived,
                long decodedFrames,
                long droppedFrames,
                long framesSubmitted,
                long decoderInputStarvation,
                long heartbeatPackets,
                long cursorUpdates,
                long reconnectCount,
                String lastReconnectReason,
                long queueDelaySamples,
                long queueDelayNanosTotal,
                long queueDelayNanosMax,
                long lastPacketReceivedNanos,
                long lastFrameReceivedNanos,
                long lastDecoderActivityNanos) {
            this.width = width;
            this.height = height;
            this.fps = fps;
            this.bitrate = bitrate;
            this.windowFps = windowFps;
            this.windowBitrate = windowBitrate;
            this.latencyMs = latencyMs;
            this.videoPacketsReceived = videoPacketsReceived;
            this.idrPacketsReceived = idrPacketsReceived;
            this.decodedFrames = decodedFrames;
            this.droppedFrames = droppedFrames;
            this.framesSubmitted = framesSubmitted;
            this.decoderInputStarvation = decoderInputStarvation;
            this.heartbeatPackets = heartbeatPackets;
            this.cursorUpdates = cursorUpdates;
            this.reconnectCount = reconnectCount;
            this.lastReconnectReason = lastReconnectReason;
            this.queueDelaySamples = queueDelaySamples;
            this.queueDelayNanosTotal = queueDelayNanosTotal;
            this.queueDelayNanosMax = queueDelayNanosMax;
            this.lastPacketReceivedNanos = lastPacketReceivedNanos;
            this.lastFrameReceivedNanos = lastFrameReceivedNanos;
            this.lastDecoderActivityNanos = lastDecoderActivityNanos;
        }

        String toLogLine(Snapshot previous, long elapsedNanos) {
            String reason = lastReconnectReason.replace('"', '\'');
            double elapsedSeconds = Math.max(1L, elapsedNanos) / 1_000_000_000.0;
            long receivedDelta = videoPacketsReceived - previous.videoPacketsReceived;
            long submittedDelta = framesSubmitted - previous.framesSubmitted;
            long decodedDelta = decodedFrames - previous.decodedFrames;
            long droppedDelta = droppedFrames - previous.droppedFrames;
            long heartbeatDelta = heartbeatPackets - previous.heartbeatPackets;
            long cursorDelta = cursorUpdates - previous.cursorUpdates;
            long queueSamplesDelta = queueDelaySamples - previous.queueDelaySamples;
            long queueNanosDelta = queueDelayNanosTotal - previous.queueDelayNanosTotal;
            double queueAverageMs = queueSamplesDelta <= 0
                    ? 0.0
                    : queueNanosDelta / (queueSamplesDelta * 1_000_000.0);
            return String.format(Locale.US,
                    "[STATS] video=%d idr=%d queue_drop=%d submitted=%d input_starvation=%d "
                            + "decoded=%d heartbeat=%d reconnect=%d last_reconnect=\"%s\" "
                            + "fps=%.1f bitrate=%.1fMbps window_s=%.1f recv_fps=%.1f "
                            + "submit_fps=%.1f decode_fps=%.1f drop_delta=%d heartbeat_delta=%d "
                            + "cursor_delta=%d queue_avg_ms=%.1f queue_max_ms=%.1f",
                    videoPacketsReceived,
                    idrPacketsReceived,
                    droppedFrames,
                    framesSubmitted,
                    decoderInputStarvation,
                    decodedFrames,
                    heartbeatPackets,
                    reconnectCount,
                    reason,
                    fps,
                    bitrate / 1_000_000.0,
                    elapsedSeconds,
                    receivedDelta / elapsedSeconds,
                    submittedDelta / elapsedSeconds,
                    decodedDelta / elapsedSeconds,
                    droppedDelta,
                    heartbeatDelta,
                    cursorDelta,
                    queueAverageMs,
                    queueDelayNanosMax / 1_000_000.0);
        }
    }
}
