package com.secondscreen.android;

import java.util.ArrayDeque;

final class LatestFrameQueue {
    private static final int MAX_PENDING_FRAMES = 1;

    private final ArrayDeque<EncodedFrame> pending = new ArrayDeque<>();
    private long droppedFrames;
    private boolean closed;

    synchronized void offer(EncodedFrame frame) {
        if (closed) {
            return;
        }
        if (pending.size() >= MAX_PENDING_FRAMES) {
            pending.removeFirst();
            droppedFrames++;
        }
        pending.addLast(frame);
        notifyAll();
    }

    synchronized long takeDroppedFrames() {
        long result = droppedFrames;
        droppedFrames = 0;
        return result;
    }

    synchronized EncodedFrame take() throws InterruptedException {
        while (pending.isEmpty() && !closed) {
            wait();
        }
        if (pending.isEmpty()) {
            return null;
        }
        EncodedFrame frame = pending.removeFirst();
        notifyAll();
        return frame;
    }

    synchronized void close() {
        closed = true;
        pending.clear();
        notifyAll();
    }
}
