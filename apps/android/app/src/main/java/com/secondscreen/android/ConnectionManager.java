package com.secondscreen.android;

import android.content.Context;
import android.os.Build;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.Surface;
import android.view.WindowManager;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

final class ConnectionManager {
    private static final String TAG = "SecondScreenConnection";
    static final String HOST = "127.0.0.1";
    static final int PORT = 5000;

    interface Listener {
        void onConnecting();
        void onConnected(ConnectionInfo info);
        void onError(String message);
        void onDisconnected();
    }

    private static final int CONNECT_TIMEOUT_MS = 2_000;
    private static final int HANDSHAKE_LINE_LIMIT = 64 * 1024;
    private static final long RECONNECT_DELAY_MS = 1_000L;

    private final Context appContext;
    private final Listener listener;
    private final Object lock = new Object();
    private ExecutorService executor;
    private volatile Surface surface;
    private volatile Socket activeSocket;
    private volatile StreamReceiver activeReceiver;
    private final StreamStats stats = new StreamStats();
    private volatile boolean streamStarted;
    private long generation;
    private boolean running;
    private boolean closed;

    ConnectionManager(Context context, Listener listener) {
        this.appContext = context.getApplicationContext();
        this.listener = listener;
    }

    void start(Surface surface) {
        if (surface == null || !surface.isValid()) {
            return;
        }
        synchronized (lock) {
            if (closed) {
                return;
            }
            this.surface = surface;
            if (running) {
                return;
            }
            running = true;
            long token = ++generation;
            executor = Executors.newSingleThreadExecutor(r -> {
                Thread thread = new Thread(r, "second-screen-connection");
                thread.setDaemon(true);
                return thread;
            });
            executor.execute(() -> runConnectionLoop(token));
        }
    }

    void stop() {
        synchronized (lock) {
            running = false;
            generation++;
            closeActiveResourcesLocked();
            if (executor != null) {
                executor.shutdownNow();
                executor = null;
            }
        }
    }

    void close() {
        synchronized (lock) {
            closed = true;
        }
        stop();
    }

    void reconnect() {
        Surface currentSurface = surface;
        stop();
        start(currentSurface);
    }

    StreamStats.Snapshot getStats() {
        return streamStarted ? stats.snapshot() : null;
    }

    private void runConnectionLoop(long token) {
        while (isCurrent(token)) {
            StreamReceiver receiver = null;
            String reconnectReason = null;
            try {
                notifyConnecting(token);
                Socket socket = connectSocket();
                activeSocket = socket;
                BufferedInputStream input = new BufferedInputStream(socket.getInputStream(), 8 * 1024);
                ConnectionInfo info = performHandshake(input, socket.getOutputStream());
                if (!isCurrent(token)) {
                    closeQuietly(socket);
                    return;
                }
                receiver = new StreamReceiver(socket, input, surface, info, stats);
                activeReceiver = receiver;
                streamStarted = true;
                notifyConnected(token, info);
                receiver.run();
            } catch (IOException | RuntimeException e) {
                if (isCurrent(token)) {
                    reconnectReason = actionableMessage(e);
                    stats.reconnect(reconnectReason);
                    Log.w(TAG, "connection ended: " + reconnectReason, e);
                    notifyError(token, reconnectReason);
                }
            } finally {
                if (receiver != null) {
                    receiver.close();
                }
                activeReceiver = null;
                closeActiveSocket();
            }

            if (!isCurrent(token)) {
                return;
            }
            if (reconnectReason == null) {
                reconnectReason = "stream ended";
                stats.reconnect(reconnectReason);
            }
            notifyDisconnected(token);
            if (!sleepBeforeReconnect(token)) {
                return;
            }
        }
    }

    private Socket connectSocket() throws IOException {
        Socket socket = new Socket();
        socket.setTcpNoDelay(true);
        socket.setKeepAlive(true);
        socket.setReceiveBufferSize(32 * 1024);
        try {
            socket.connect(new InetSocketAddress(HOST, PORT), CONNECT_TIMEOUT_MS);
            return socket;
        } catch (IOException e) {
            closeQuietly(socket);
            throw new IOException("Cannot connect to " + HOST + ":" + PORT
                    + "; run `adb reverse tcp:5000 tcp:5000` and start the host", e);
        }
    }

    private ConnectionInfo performHandshake(BufferedInputStream input, OutputStream output)
            throws IOException {
        DisplayMetrics metrics = new DisplayMetrics();
        WindowManager windowManager = (WindowManager) appContext.getSystemService(Context.WINDOW_SERVICE);
        Display display = windowManager == null ? null : windowManager.getDefaultDisplay();
        if (display != null) {
            display.getRealMetrics(metrics);
        }
        int width = Math.max(1, metrics.widthPixels);
        int height = Math.max(1, metrics.heightPixels);
        if (width < height) {
            int swap = width;
            width = height;
            height = swap;
        }

        JSONObject request = new JSONObject();
        try {
            request.put("protocol", 1);
            request.put("device", deviceName());
            request.put("width", width);
            request.put("height", height);
            JSONArray decoderList = new JSONArray();
            decoderList.put("h264");
            request.put("decoder", decoderList);
        } catch (Exception e) {
            throw new IOException("Unable to construct handshake JSON", e);
        }
        byte[] requestBytes = (request.toString() + "\n").getBytes(StandardCharsets.UTF_8);
        output.write(requestBytes);
        output.flush();

        String line = readLine(input);
        final JSONObject response;
        try {
            response = new JSONObject(line);
        } catch (Exception e) {
            throw new ProtocolException("Host handshake is not valid JSON: " + e.getMessage());
        }
        String codec = response.optString("codec", "");
        int responseWidth = response.optInt("width", 0);
        int responseHeight = response.optInt("height", 0);
        int fps = response.optInt("fps", 0);
        int bitrate = response.optInt("bitrate", 0);
        if (!"h264".equalsIgnoreCase(codec)) {
            throw new ProtocolException("Host selected unsupported codec: " + codec);
        }
        if (responseWidth <= 0 || responseHeight <= 0
                || responseWidth > 16_384 || responseHeight > 16_384) {
            throw new ProtocolException("Host returned invalid video dimensions: "
                    + responseWidth + "x" + responseHeight);
        }
        if (fps <= 0 || fps > 240) {
            throw new ProtocolException("Host returned invalid frame rate: " + fps);
        }
        if (bitrate <= 0) {
            throw new ProtocolException("Host returned invalid bitrate: " + bitrate);
        }
        return new ConnectionInfo("h264", responseWidth, responseHeight, fps, bitrate);
    }

    private static String readLine(InputStream input) throws IOException {
        ByteArrayOutputStream line = new ByteArrayOutputStream();
        while (line.size() < HANDSHAKE_LINE_LIMIT) {
            int value = input.read();
            if (value < 0) {
                throw new IOException("Host closed the connection during handshake");
            }
            if (value == '\n') {
                return line.toString(StandardCharsets.UTF_8.name()).trim();
            }
            line.write(value);
        }
        throw new ProtocolException("Host handshake exceeds " + HANDSHAKE_LINE_LIMIT + " bytes");
    }

    private String deviceName() {
        String manufacturer = Build.MANUFACTURER == null ? "Android" : Build.MANUFACTURER.trim();
        String model = Build.MODEL == null ? "device" : Build.MODEL.trim();
        if (manufacturer.isEmpty()) {
            return model;
        }
        if (model.isEmpty()) {
            return manufacturer;
        }
        return manufacturer + " " + model;
    }

    private boolean sleepBeforeReconnect(long token) {
        try {
            Thread.sleep(RECONNECT_DELAY_MS);
            return isCurrent(token);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    private boolean isCurrent(long token) {
        synchronized (lock) {
            return !closed && running && generation == token;
        }
    }

    private void notifyConnecting(long token) {
        if (isCurrent(token)) {
            listener.onConnecting();
        }
    }

    private void notifyConnected(long token, ConnectionInfo info) {
        if (isCurrent(token)) {
            listener.onConnected(info);
        }
    }

    private void notifyError(long token, String message) {
        if (isCurrent(token)) {
            listener.onError(message);
        }
    }

    private void notifyDisconnected(long token) {
        if (isCurrent(token)) {
            listener.onDisconnected();
        }
    }

    private void closeActiveResourcesLocked() {
        StreamReceiver receiver = activeReceiver;
        activeReceiver = null;
        if (receiver != null) {
            receiver.close();
        }
        closeActiveSocket();
    }

    private void closeActiveSocket() {
        Socket socket = activeSocket;
        activeSocket = null;
        closeQuietly(socket);
    }

    private static String actionableMessage(Throwable error) {
        String message = error.getMessage();
        return message == null || message.trim().isEmpty()
                ? "Connection failed: " + error.getClass().getSimpleName()
                : message;
    }

    private static void closeQuietly(Socket socket) {
        if (socket == null) {
            return;
        }
        try {
            socket.close();
        } catch (IOException ignored) {
            // Best effort during reconnect and lifecycle teardown.
        }
    }
}
