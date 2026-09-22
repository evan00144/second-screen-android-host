package com.secondscreen.android;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Build;
import android.view.Surface;

import java.io.IOException;
import java.nio.ByteBuffer;

final class VideoDecoder {
    private static final long INPUT_TIMEOUT_US = 5_000L;
    private static final int MAX_ACCESS_UNIT_BYTES = 16 * 1024 * 1024;

    private final ConnectionInfo connectionInfo;
    private final Surface surface;
    private final StreamStats stats;
    private MediaCodec codec;
    private long lastPresentationTimeUs;
    private long lastFrameId = -1L;
    private boolean waitingForRandomAccess;

    VideoDecoder(ConnectionInfo connectionInfo, Surface surface, StreamStats stats) {
        this.connectionInfo = connectionInfo;
        this.surface = surface;
        this.stats = stats;
    }

    void start(byte[] firstAccessUnit) throws IOException {
        if (connectionInfo.width <= 0 || connectionInfo.height <= 0) {
            throw new ProtocolException("Host returned invalid video dimensions: "
                    + connectionInfo.width + "x" + connectionInfo.height);
        }
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                surface.setFrameRate(
                        connectionInfo.fps,
                        Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                        0);
            }
            codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
            MediaFormat format = MediaFormat.createVideoFormat(
                    MediaFormat.MIMETYPE_VIDEO_AVC,
                    connectionInfo.width,
                    connectionInfo.height);
            format.setInteger(MediaFormat.KEY_FRAME_RATE, connectionInfo.fps);
            format.setInteger(MediaFormat.KEY_OPERATING_RATE, connectionInfo.fps);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                format.setInteger(MediaFormat.KEY_ALLOW_FRAME_DROP, 1);
            }
            format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, MAX_ACCESS_UNIT_BYTES);
            byte[] sps = findNalUnit(firstAccessUnit, 7);
            byte[] pps = findNalUnit(firstAccessUnit, 8);
            if (sps != null) {
                format.setByteBuffer("csd-0", ByteBuffer.wrap(sps));
            }
            if (pps != null) {
                format.setByteBuffer("csd-1", ByteBuffer.wrap(pps));
            }
            codec.configure(format, surface, null, 0);
            codec.start();
        } catch (Exception e) {
            release();
            throw new IOException("Unable to start H.264 MediaCodec: " + e.getMessage(), e);
        }
    }

    static byte[] findNalUnit(byte[] accessUnit, int requestedType) {
        int start = 0;
        while (start + 4 < accessUnit.length) {
            int startCodeLength = 0;
            if (accessUnit[start] == 0 && accessUnit[start + 1] == 0
                    && accessUnit[start + 2] == 1) {
                startCodeLength = 3;
            } else if (accessUnit[start] == 0 && accessUnit[start + 1] == 0
                    && accessUnit[start + 2] == 0 && accessUnit[start + 3] == 1) {
                startCodeLength = 4;
            }
            if (startCodeLength == 0) {
                start++;
                continue;
            }
            int nalStart = start + startCodeLength;
            int next = nalStart;
            while (next + 3 < accessUnit.length
                    && !(accessUnit[next] == 0 && accessUnit[next + 1] == 0
                    && (accessUnit[next + 2] == 1
                    || (accessUnit[next + 2] == 0 && accessUnit[next + 3] == 1)))) {
                next++;
            }
            int nalEnd = next + 3 < accessUnit.length ? next : accessUnit.length;
            if (nalStart < nalEnd && (accessUnit[nalStart] & 0x1f) == requestedType) {
                byte[] nal = new byte[4 + nalEnd - nalStart];
                nal[3] = 1;
                System.arraycopy(accessUnit, nalStart, nal, 4, nalEnd - nalStart);
                return nal;
            }
            start = nalEnd;
        }
        return null;
    }

    boolean decode(EncodedFrame frame) throws IOException {
        if (codec == null) {
            throw new IOException("H.264 decoder is not started");
        }
        stats.decoderActivity();
        if (lastFrameId >= 0 && frame.frameId > lastFrameId + 1L) {
            waitingForRandomAccess = true;
        }
        lastFrameId = frame.frameId;
        if (waitingForRandomAccess && !frame.randomAccess) {
            return false;
        }
        if (frame.randomAccess) {
            waitingForRandomAccess = false;
        }
        drainOutput();
        int inputIndex;
        try {
            inputIndex = codec.dequeueInputBuffer(INPUT_TIMEOUT_US);
        } catch (RuntimeException e) {
            throw new IOException("MediaCodec input dequeue failed: " + e.getMessage(), e);
        }
        if (inputIndex < 0) {
            return false;
        }

        ByteBuffer inputBuffer = codec.getInputBuffer(inputIndex);
        if (inputBuffer == null) {
            throw new IOException("MediaCodec returned a null input buffer");
        }
        if (frame.accessUnit.length > inputBuffer.capacity()) {
            throw new IOException("H.264 access unit is " + frame.accessUnit.length
                    + " bytes; decoder input capacity is " + inputBuffer.capacity());
        }
        inputBuffer.clear();
        inputBuffer.put(frame.accessUnit);
        try {
            long nowUs = System.nanoTime() / 1_000L;
            long presentationTimeUs = Math.max(lastPresentationTimeUs + 1L, nowUs);
            codec.queueInputBuffer(inputIndex, 0, frame.accessUnit.length, presentationTimeUs, 0);
            lastPresentationTimeUs = presentationTimeUs;
        } catch (RuntimeException e) {
            throw new IOException("MediaCodec input queue failed: " + e.getMessage(), e);
        }
        drainOutput();
        return true;
    }

    private void drainOutput() throws IOException {
        MediaCodec.BufferInfo bufferInfo = new MediaCodec.BufferInfo();
        int pendingOutputIndex = -1;
        while (true) {
            final int outputIndex;
            try {
                outputIndex = codec.dequeueOutputBuffer(bufferInfo, 0);
            } catch (RuntimeException e) {
                throw new IOException("MediaCodec output dequeue failed: " + e.getMessage(), e);
            }
            if (outputIndex == MediaCodec.INFO_TRY_AGAIN_LATER) {
                break;
            }
            if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                continue;
            }
            if (outputIndex == MediaCodec.INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;
            }
            if (outputIndex >= 0) {
                if (pendingOutputIndex >= 0) {
                    try {
                        codec.releaseOutputBuffer(pendingOutputIndex, false);
                    } catch (RuntimeException e) {
                        throw new IOException("MediaCodec stale output release failed: "
                                + e.getMessage(), e);
                    }
                    stats.frameDecoded();
                }
                pendingOutputIndex = outputIndex;
            }
        }
        if (pendingOutputIndex >= 0) {
            try {
                codec.releaseOutputBuffer(pendingOutputIndex, true);
            } catch (RuntimeException e) {
                throw new IOException("MediaCodec output release failed: " + e.getMessage(), e);
            }
            stats.frameDecoded();
        }
    }

    void release() {
        if (codec == null) {
            return;
        }
        try {
            codec.stop();
        } catch (RuntimeException ignored) {
            // The decoder may already have failed or been stopped by the platform.
        }
        try {
            codec.release();
        } catch (RuntimeException ignored) {
            // Resource cleanup must remain best effort during lifecycle teardown.
        }
        codec = null;
    }
}
