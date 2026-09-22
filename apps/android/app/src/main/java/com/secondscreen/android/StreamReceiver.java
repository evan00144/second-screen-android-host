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
    private static final int MAX_ACCESS_UNIT_BYTES = 16 * 1024 * 1024;
    private static final long WATCHDOG_INTERVAL_MS = 1_000L;
    private static final long STALL_TIMEOUT_NANOS = 5_000_000_000L;

    private final Socket socket;
    private final InputStream input;
    private final Surface surface;
    private final ConnectionInfo connectionInfo;
    private final LatestFrameQueue queue = new LatestFrameQueue();
    private final StreamStats stats = new StreamStats();
    private final AtomicReference<Throwable> decoderFailure = new AtomicReference<>();
    private volatile boolean closed;
    private volatile boolean stopRequested;
    private Thread decoderThread;
    private Thread watchdogThread;

    StreamReceiver(Socket socket, InputStream input, Surface surface, ConnectionInfo connectionInfo) {
        this.socket = socket;
        this.input = input;
        this.surface = surface;
        this.connectionInfo = connectionInfo;
        stats.setFormat(connectionInfo.width, connectionInfo.height);
    }

    void run() throws IOException {
        VideoDecoder decoder = new VideoDecoder(connectionInfo, surface, stats);
        EncodedFrame firstFrame = readPacket(input);
        stats.packetReceived(firstFrame.accessUnit.length, firstFrame.captureTimestampUs);
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
                stats.packetReceived(frame.accessUnit.length, frame.captureTimestampUs);
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
        try {
            while (!closed) {
                Thread.sleep(WATCHDOG_INTERVAL_MS);
                if (closed) {
                    return;
                }
                StreamStats.Snapshot snapshot = stats.snapshot();
                long now = System.nanoTime();
                if (now - snapshot.lastFrameReceivedNanos > STALL_TIMEOUT_NANOS) {
                    failStream("No video packet received for 5 seconds");
                    return;
                }
                if (snapshot.receivedFrames > 1
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

    private static EncodedFrame readPacket(InputStream input) throws IOException {
        byte[] header = new byte[HEADER_BYTES];
        readFully(input, header, 0, header.length);
        ByteBuffer buffer = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        int magic = buffer.getInt();
        if (magic != MAGIC) {
            throw new ProtocolException(String.format(
                    "Invalid SSV1 packet magic 0x%08x (expected 0x%08x)", magic, MAGIC));
        }
        long unsignedLength = Integer.toUnsignedLong(buffer.getInt());
        if (unsignedLength <= 0 || unsignedLength > MAX_ACCESS_UNIT_BYTES) {
            throw new ProtocolException("Invalid SSV1 access-unit length: " + unsignedLength
                    + " (maximum " + MAX_ACCESS_UNIT_BYTES + ")");
        }
        long frameId = buffer.getLong();
        long captureTimestampUs = buffer.getLong();
        byte[] accessUnit = new byte[(int) unsignedLength];
        readFully(input, accessUnit, 0, accessUnit.length);
        return new EncodedFrame(frameId, captureTimestampUs, VideoDecoder.findNalUnit(accessUnit, 5) != null, accessUnit);
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
