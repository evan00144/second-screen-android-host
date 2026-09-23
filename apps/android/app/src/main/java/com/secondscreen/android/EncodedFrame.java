package com.secondscreen.android;

final class EncodedFrame {
    final long frameId;
    final long captureTimestampUs;
    final long receivedNanos;
    final boolean randomAccess;
    final boolean heartbeat;
    final boolean control;
    final int controlType;
    final byte[] controlPayload;
    final byte[] accessUnit;

    EncodedFrame(
            long frameId,
            long captureTimestampUs,
            long receivedNanos,
            boolean randomAccess,
            boolean heartbeat,
            boolean control,
            int controlType,
            byte[] controlPayload,
            byte[] accessUnit) {
        this.frameId = frameId;
        this.captureTimestampUs = captureTimestampUs;
        this.receivedNanos = receivedNanos;
        this.randomAccess = randomAccess;
        this.heartbeat = heartbeat;
        this.control = control;
        this.controlType = controlType;
        this.controlPayload = controlPayload;
        this.accessUnit = accessUnit;
    }
}
