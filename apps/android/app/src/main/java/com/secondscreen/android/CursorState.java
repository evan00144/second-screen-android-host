package com.secondscreen.android;

final class CursorState {
    final boolean visible;
    final int x;
    final int y;
    final int width;
    final int height;
    final long sequence;

    CursorState(boolean visible, int x, int y, int width, int height, long sequence) {
        this.visible = visible;
        this.x = x;
        this.y = y;
        this.width = width;
        this.height = height;
        this.sequence = sequence;
    }
}
