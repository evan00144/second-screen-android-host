package com.secondscreen.android;

final class EncodedFrame {
    final long frameId;
    final long captureTimestampUs;
    final long receivedNanos;
    final boolean randomAccess;
    final boolean heartbeat;
    final byte[] accessUnit;

    EncodedFrame(
            long frameId,
            long captureTimestampUs,
            long receivedNanos,
            boolean randomAccess,
            boolean heartbeat,
            byte[] accessUnit) {
        this.frameId = frameId;
        this.captureTimestampUs = captureTimestampUs;
        this.receivedNanos = receivedNanos;
        this.randomAccess = randomAccess;
        this.heartbeat = heartbeat;
        this.accessUnit = accessUnit;
    }
}
