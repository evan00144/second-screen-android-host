package com.secondscreen.android;

import java.util.concurrent.atomic.AtomicLong;

final class StreamStats {
    private final AtomicLong receivedFrames = new AtomicLong();
    private final AtomicLong decodedFrames = new AtomicLong();
    private final AtomicLong droppedFrames = new AtomicLong();
    private final AtomicLong receivedBytes = new AtomicLong();
    private volatile long lastCaptureTimestampUs;
    private volatile long lastFrameReceivedNanos;
    private volatile long lastDecoderActivityNanos;
    private volatile long lastSurfaceRenderNanos;
    private volatile long startedNanos = System.nanoTime();
    private volatile int width;
    private volatile int height;

    void setFormat(int width, int height) {
        this.width = width;
        this.height = height;
    }

    void packetReceived(int bytes, long captureTimestampUs) {
        receivedFrames.incrementAndGet();
        receivedBytes.addAndGet(bytes);
        lastCaptureTimestampUs = captureTimestampUs;
        lastFrameReceivedNanos = System.nanoTime();
    }

    void frameDecoded() {
        decodedFrames.incrementAndGet();
    }

    void decoderActivity() {
        lastDecoderActivityNanos = System.nanoTime();
    }

    void frameRendered() {
        lastSurfaceRenderNanos = System.nanoTime();
    }

    void framesDropped(long count) {
        if (count > 0) {
            droppedFrames.addAndGet(count);
        }
    }

    Snapshot snapshot() {
        long nowNanos = System.nanoTime();
        long elapsedNanos = Math.max(1L, nowNanos - startedNanos);
        double seconds = elapsedNanos / 1_000_000_000.0;
        double fps = decodedFrames.get() / seconds;
        double bitrate = receivedBytes.get() * 8.0 / seconds;

        // Host timestamps use QPC/boot time; Android wall-clock time is not comparable.
        long latencyMs = -1L;
        return new Snapshot(
                width,
                height,
                fps,
                bitrate,
                latencyMs,
                receivedFrames.get(),
                decodedFrames.get(),
                droppedFrames.get(),
                lastFrameReceivedNanos,
                lastDecoderActivityNanos,
                lastSurfaceRenderNanos);
    }

    static final class Snapshot {
        final int width;
        final int height;
        final double fps;
        final double bitrate;
        final long latencyMs;
        final long receivedFrames;
        final long decodedFrames;
        final long droppedFrames;
        final long lastFrameReceivedNanos;
        final long lastDecoderActivityNanos;
        final long lastSurfaceRenderNanos;

        Snapshot(
                int width,
                int height,
                double fps,
                double bitrate,
                long latencyMs,
                long receivedFrames,
                long decodedFrames,
                long droppedFrames,
                long lastFrameReceivedNanos,
                long lastDecoderActivityNanos,
                long lastSurfaceRenderNanos) {
            this.width = width;
            this.height = height;
            this.fps = fps;
            this.bitrate = bitrate;
            this.latencyMs = latencyMs;
            this.receivedFrames = receivedFrames;
            this.decodedFrames = decodedFrames;
            this.droppedFrames = droppedFrames;
            this.lastFrameReceivedNanos = lastFrameReceivedNanos;
            this.lastDecoderActivityNanos = lastDecoderActivityNanos;
            this.lastSurfaceRenderNanos = lastSurfaceRenderNanos;
        }
    }
}
