package com.secondscreen.android;

final class EncodedFrame {
    final long frameId;
    final long captureTimestampUs;
    final boolean randomAccess;
    final byte[] accessUnit;

    EncodedFrame(long frameId, long captureTimestampUs, boolean randomAccess, byte[] accessUnit) {
        this.frameId = frameId;
        this.captureTimestampUs = captureTimestampUs;
        this.randomAccess = randomAccess;
        this.accessUnit = accessUnit;
    }
}
