package com.secondscreen.android;

import android.util.Log;
import android.view.Surface;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicReference;

final class StreamReceiver {
    private static final String TAG = "SecondScreenStream";
    private static final int HEADER_BYTES = 24;
    private static final int MAGIC = 0x31565353;
    private static final int CONTROL_LENGTH_FLAG = 0x80000000;
    private static final int CURSOR_CONTROL_TYPE = 1;
    private static final int CURSOR_PAYLOAD_BYTES = 32;
    private static final int MAX_ACCESS_UNIT_BYTES = 16 * 1024 * 1024;
    private static final long WATCHDOG_INTERVAL_MS = 1_000L;
    private static final long STALL_TIMEOUT_NANOS = 5_000_000_000L;
    private static final long STATS_INTERVAL_NANOS = 5_000_000_000L;

    private final Socket socket;
    private final InputStream input;
    private final Surface surface;
    private final ConnectionInfo connectionInfo;
    private final LatestFrameQueue queue = new LatestFrameQueue();
    private final StreamStats stats;
    private final CursorListener cursorListener;
    private final AtomicReference<Throwable> decoderFailure = new AtomicReference<>();
    private volatile boolean closed;
    private volatile boolean stopRequested;
    private Thread decoderThread;
    private Thread watchdogThread;

    StreamReceiver(
            Socket socket,
            InputStream input,
            Surface surface,
            ConnectionInfo connectionInfo,
            StreamStats stats,
            CursorListener cursorListener) {
        this.socket = socket;
        this.input = input;
        this.surface = surface;
        this.connectionInfo = connectionInfo;
        this.stats = stats;
        this.cursorListener = cursorListener;
        stats.beginStream(connectionInfo.width, connectionInfo.height);
    }

    void run() throws IOException {
        VideoDecoder decoder = new VideoDecoder(connectionInfo, surface, stats);
        EncodedFrame firstFrame = readVideoPacket(input);
        stats.packetReceived(
                firstFrame.accessUnit.length,
                firstFrame.captureTimestampUs,
                firstFrame.randomAccess);
        decoder.start(firstFrame.accessUnit);
        decoderThread = new Thread(() -> decodeLoop(decoder), "second-screen-decoder");
        decoderThread.start();
        queue.offer(firstFrame);
        stats.framesDropped(queue.takeDroppedFrames());
        watchdogThread = new Thread(this::watchdogLoop, "second-screen-watchdog");
        watchdogThread.start();

        IOException terminalError = null;
        try {
            while (!closed) {
                Throwable failure = decoderFailure.get();
                if (failure != null) {
                    throw asIOException("H.264 decoder stopped", failure);
                }
                EncodedFrame frame = readPacket(input);
                if (frame.heartbeat) {
                    stats.heartbeatReceived();
                    continue;
                }
                if (frame.control) {
                    dispatchControl(frame);
                    continue;
                }
                stats.packetReceived(
                        frame.accessUnit.length,
                        frame.captureTimestampUs,
                        frame.randomAccess);
                queue.offer(frame);
                stats.framesDropped(queue.takeDroppedFrames());
            }
        } catch (IOException e) {
            if (!stopRequested) {
                terminalError = e;
            }
        } finally {
            closed = true;
            queue.close();
            closeSocket();
            if (decoderThread != null && decoderThread != Thread.currentThread()) {
                decoderThread.interrupt();
                try {
                    decoderThread.join(1_000L);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
            if (watchdogThread != null && watchdogThread != Thread.currentThread()) {
                watchdogThread.interrupt();
                try {
                    watchdogThread.join(1_000L);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
        }

        Throwable failure = decoderFailure.get();
        if (!stopRequested && failure != null) {
            throw asIOException("H.264 decoder stopped", failure);
        }
        if (terminalError != null) {
            throw terminalError;
        }
    }

    private void decodeLoop(VideoDecoder decoder) {
        try {
            while (!closed) {
                EncodedFrame frame = queue.take();
                if (frame == null) {
                    return;
                }
                decoder.decode(frame);
            }
        } catch (InterruptedException e) {
            if (!stopRequested) {
                decoderFailure.compareAndSet(null, e);
            }
            Thread.currentThread().interrupt();
        } catch (Throwable t) {
            if (!stopRequested) {
                decoderFailure.compareAndSet(null, t);
                Log.e(TAG, "H.264 decoder stopped", t);
            }
            closed = true;
            queue.close();
            closeSocket();
        } finally {
            decoder.release();
        }
    }

    private void watchdogLoop() {
        long nextStatsLogNanos = System.nanoTime() + STATS_INTERVAL_NANOS;
        StreamStats.Snapshot previousStats = stats.snapshot();
        long previousStatsNanos = System.nanoTime();
        try {
            while (!closed) {
                Thread.sleep(WATCHDOG_INTERVAL_MS);
                if (closed) {
                    return;
                }
                StreamStats.Snapshot snapshot = stats.snapshot();
                long now = System.nanoTime();
                if (now >= nextStatsLogNanos) {
                    Log.i(TAG, snapshot.toLogLine(previousStats, now - previousStatsNanos));
                    previousStats = snapshot;
                    previousStatsNanos = now;
                    nextStatsLogNanos = now + STATS_INTERVAL_NANOS;
                }
                if (snapshot.lastPacketReceivedNanos == 0
                        || now - snapshot.lastPacketReceivedNanos > STALL_TIMEOUT_NANOS) {
                    failStream("No stream packet received for 5 seconds");
                    return;
                }
                boolean recentVideo = snapshot.lastFrameReceivedNanos != 0
                        && now - snapshot.lastFrameReceivedNanos <= STALL_TIMEOUT_NANOS;
                if (recentVideo && snapshot.videoPacketsReceived > 1
                        && (snapshot.lastDecoderActivityNanos == 0
                        || now - snapshot.lastDecoderActivityNanos > STALL_TIMEOUT_NANOS)) {
                    failStream("Video decoder stopped processing for 5 seconds");
                    return;
                }

            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private void failStream(String message) {
        decoderFailure.compareAndSet(null, new IOException(message));
        Log.w(TAG, message);
        closed = true;
        queue.close();
        closeSocket();
    }

    StreamStats.Snapshot getStats() {
        return stats.snapshot();
    }

    void close() {
        stopRequested = true;
        closed = true;
        queue.close();
        closeSocket();
        if (decoderThread != null) {
            decoderThread.interrupt();
        }
    }

    private void closeSocket() {
        try {
            socket.close();
        } catch (IOException ignored) {
            // Closing a socket is idempotent for lifecycle teardown.
        }
    }

    private void dispatchControl(EncodedFrame frame) throws IOException {
        if (frame.controlType != CURSOR_CONTROL_TYPE) {
            return;
        }
        if (frame.controlPayload.length != CURSOR_PAYLOAD_BYTES) {
            throw new ProtocolException("Invalid cursor control payload length: "
                    + frame.controlPayload.length);
        }
        ByteBuffer payload = ByteBuffer.wrap(frame.controlPayload).order(ByteOrder.LITTLE_ENDIAN);
        int version = payload.getInt();
        boolean visible = payload.getInt() != 0;
        int x = payload.getInt();
        int y = payload.getInt();
        int width = payload.getInt();
        int height = payload.getInt();
        long sequence = payload.getLong();
        if (version != 1 || width <= 0 || height <= 0 || width > 16_384 || height > 16_384) {
            throw new ProtocolException("Invalid cursor control state");
        }
        stats.cursorUpdateReceived();
        if (cursorListener != null) {
            cursorListener.onCursor(new CursorState(visible, x, y, width, height, sequence));
        }
    }

    private static EncodedFrame readPacket(InputStream input) throws IOException {
        byte[] header = new byte[HEADER_BYTES];
        readFully(input, header, 0, header.length);
        ByteBuffer buffer = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        int magic = buffer.getInt();
        if (magic != MAGIC) {
            throw new ProtocolException(String.format(
                    "Invalid SSV1 packet magic 0x%08x (expected 0x%08x)", magic, MAGIC));
        }
        int rawLength = buffer.getInt();
        boolean control = (rawLength & CONTROL_LENGTH_FLAG) != 0;
        long unsignedLength = Integer.toUnsignedLong(rawLength & ~CONTROL_LENGTH_FLAG);
        if (unsignedLength > MAX_ACCESS_UNIT_BYTES) {
            throw new ProtocolException("Invalid SSV1 access-unit length: " + unsignedLength
                    + " (maximum " + MAX_ACCESS_UNIT_BYTES + ")");
        }
        long frameId = buffer.getLong();
        long captureTimestampUs = buffer.getLong();
        if (unsignedLength == 0) {
            if (frameId != 0) {
                throw new ProtocolException("Invalid SSV1 heartbeat frame ID: " + frameId);
            }
            return new EncodedFrame(
                    frameId,
                    captureTimestampUs,
                    System.nanoTime(),
                    false,
                    true,
                    false,
                    0,
                    new byte[0],
                    new byte[0]);
        }
        byte[] payload = new byte[(int) unsignedLength];
        readFully(input, payload, 0, payload.length);
        if (control) {
            if (payload.length < 4) {
                throw new ProtocolException("Control packet is missing its type");
            }
            ByteBuffer controlBuffer = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
            int controlType = controlBuffer.getInt();
            byte[] controlPayload = new byte[payload.length - 4];
            System.arraycopy(payload, 4, controlPayload, 0, controlPayload.length);
            return new EncodedFrame(
                    frameId,
                    captureTimestampUs,
                    System.nanoTime(),
                    false,
                    false,
                    true,
                    controlType,
                    controlPayload,
                    new byte[0]);
        }
        return new EncodedFrame(
                frameId,
                captureTimestampUs,
                System.nanoTime(),
                VideoDecoder.findNalUnit(payload, 5) != null,
                false,
                false,
                0,
                new byte[0],
                payload);
    }

    private EncodedFrame readVideoPacket(InputStream input) throws IOException {
        while (true) {
            EncodedFrame frame = readPacket(input);
            if (frame.heartbeat) {
                stats.heartbeatReceived();
                continue;
            }
            if (frame.control) {
                dispatchControl(frame);
                continue;
            }
            return frame;
        }
    }

    interface CursorListener {
        void onCursor(CursorState state);
    }

    private static void readFully(InputStream input, byte[] buffer, int offset, int length)
            throws IOException {
        int total = 0;
        while (total < length) {
            int count = input.read(buffer, offset + total, length - total);
            if (count < 0) {
                throw new EOFException("Stream ended after " + total + " of " + length
                        + " packet bytes");
            }
            if (count == 0) {
                continue;
            }
            total += count;
        }
    }

    private static IOException asIOException(String message, Throwable failure) {
        if (failure instanceof IOException) {
            return (IOException) failure;
        }
        String detail = failure.getMessage();
        return new IOException(message + (detail == null ? "" : ": " + detail), failure);
    }
}
