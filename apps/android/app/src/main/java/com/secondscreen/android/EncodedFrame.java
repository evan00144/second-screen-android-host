package com.secondscreen.android;

final class EncodedFrame {
    final long frameId;
    final long captureTimestampUs;
    final boolean randomAccess;
    final boolean heartbeat;
    final byte[] accessUnit;

    EncodedFrame(
            long frameId,
            long captureTimestampUs,
            boolean randomAccess,
            boolean heartbeat,
            byte[] accessUnit) {
        this.frameId = frameId;
        this.captureTimestampUs = captureTimestampUs;
        this.randomAccess = randomAccess;
        this.heartbeat = heartbeat;
        this.accessUnit = accessUnit;
    }
}
