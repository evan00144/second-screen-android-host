package com.secondscreen.android;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.util.Log;

import java.util.Locale;

public final class MainActivity extends Activity implements ConnectionManager.Listener {
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final Runnable statsUpdater = new Runnable() {
        @Override
        public void run() {
            updateStats();
            if (!isFinishing()) {
                mainHandler.postDelayed(this, 500L);
            }
        }
    };

    private ConnectionManager connectionManager;
    private SurfaceView surfaceView;
    private CursorOverlayView cursorOverlayView;
    private TextView statusView;
    private TextView statsView;
    private Button retryButton;
    private boolean statsVisible;
    private boolean surfaceReady;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i("SecondScreenBuild", "version=" + BuildConfig.VERSION_NAME
                + " versionCode=" + BuildConfig.VERSION_CODE + " phase=16");
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        connectionManager = new ConnectionManager(this, this);
        buildContentView();
        configureImmersiveMode();
        mainHandler.post(statsUpdater);
    }

    private void buildContentView() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);

        surfaceView = new SurfaceView(this);
        surfaceView.setFocusable(false);
        surfaceView.setOnClickListener(view -> toggleStats());
        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                surfaceReady = true;
                connectionManager.start(holder.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                if (surfaceReady) {
                    connectionManager.start(holder.getSurface());
                }
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                surfaceReady = false;
                connectionManager.stop();
            }
        });
        root.addView(surfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        cursorOverlayView = new CursorOverlayView(this);
        root.addView(cursorOverlayView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        statusView = new TextView(this);
        statusView.setTextColor(Color.WHITE);
        statusView.setTextSize(16f);
        statusView.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        statusView.setGravity(Gravity.CENTER);
        statusView.setPadding(32, 18, 32, 18);
        statusView.setText("Waiting for display surface…");
        statusView.setBackgroundColor(0xCC202124);
        FrameLayout.LayoutParams statusParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER);
        root.addView(statusView, statusParams);

        retryButton = new Button(this);
        retryButton.setText("RECONNECT");
        retryButton.setOnClickListener(view -> {
            if (surfaceReady) {
                connectionManager.reconnect();
            }
        });
        retryButton.setVisibility(View.GONE);
        FrameLayout.LayoutParams retryParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER_HORIZONTAL | Gravity.BOTTOM);
        retryParams.bottomMargin = 48;
        root.addView(retryButton, retryParams);

        statsView = new TextView(this);
        statsView.setTextColor(Color.WHITE);
        statsView.setTextSize(13f);
        statsView.setTypeface(Typeface.MONOSPACE);
        statsView.setGravity(Gravity.START);
        statsView.setPadding(16, 12, 16, 12);
        statsView.setBackgroundColor(0xB0000000);
        statsView.setVisibility(View.GONE);
        FrameLayout.LayoutParams statsParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.START);
        statsParams.topMargin = 12;
        statsParams.leftMargin = 12;
        root.addView(statsView, statsParams);

        setContentView(root);
    }

    private void configureImmersiveMode() {
        Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        window.setStatusBarColor(Color.BLACK);
        window.setNavigationBarColor(Color.BLACK);
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            window.getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    private void toggleStats() {
        statsVisible = !statsVisible;
        statsView.setVisibility(statsVisible ? View.VISIBLE : View.GONE);
    }

    private void updateStats() {
        if (!statsVisible || connectionManager == null) {
            return;
        }
        StreamStats.Snapshot snapshot = connectionManager.getStats();
        if (snapshot == null) {
            statsView.setText("no stream");
            return;
        }
        String latency = snapshot.latencyMs < 0 ? "--" : Long.toString(snapshot.latencyMs);
        statsView.setText(String.format(Locale.US,
                "%.0f FPS\n%s ms\n%.1f Mbps\n%d×%d\ndrop %d",
                snapshot.windowFps,
                latency,
                snapshot.windowBitrate / 1_000_000.0,
                snapshot.width,
                snapshot.height,
                snapshot.droppedFrames));
    }

    @Override
    public void onConnecting() {
        runOnUiThread(() -> {
            statusView.setText("Connecting to 127.0.0.1:5000…");
            statusView.setVisibility(View.VISIBLE);
            retryButton.setVisibility(View.GONE);
        });
    }

    @Override
    public void onConnected(ConnectionInfo info) {
        runOnUiThread(() -> {
            cursorOverlayView.setStreamSize(info.width, info.height);
            statusView.setVisibility(View.GONE);
            retryButton.setVisibility(View.GONE);
        });
    }

    @Override
    public void onCursor(CursorState state) {
        runOnUiThread(() -> cursorOverlayView.updateCursor(state));
    }

    @Override
    public void onError(String message) {
        runOnUiThread(() -> {
            statusView.setText("Connection error\n" + message);
            statusView.setVisibility(View.VISIBLE);
            retryButton.setVisibility(View.VISIBLE);
        });
    }

    @Override
    public void onDisconnected() {
        runOnUiThread(() -> {
            cursorOverlayView.clearCursor();
            statusView.setText("Host disconnected; retrying…");
            statusView.setVisibility(View.VISIBLE);
            retryButton.setVisibility(View.VISIBLE);
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        configureImmersiveMode();
        if (surfaceReady && surfaceView != null) {
            connectionManager.start(surfaceView.getHolder().getSurface());
        }
    }

    @Override
    protected void onPause() {
        if (connectionManager != null) {
            connectionManager.stop();
        }
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        mainHandler.removeCallbacks(statsUpdater);
        if (connectionManager != null) {
            connectionManager.close();
        }
        super.onDestroy();
    }
}
