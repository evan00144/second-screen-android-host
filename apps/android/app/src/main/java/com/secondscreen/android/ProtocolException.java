package com.secondscreen.android;

import java.io.IOException;

/** Protocol violation at the host/client trust boundary. */
final class ProtocolException extends IOException {
    ProtocolException(String message) {
        super(message);
    }
}
