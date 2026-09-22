package com.secondscreen.android;

final class ConnectionInfo {
    final String codec;
    final int width;
    final int height;
    final int fps;
    final int bitrate;

    ConnectionInfo(String codec, int width, int height, int fps, int bitrate) {
        this.codec = codec;
        this.width = width;
        this.height = height;
        this.fps = fps;
        this.bitrate = bitrate;
    }
}
