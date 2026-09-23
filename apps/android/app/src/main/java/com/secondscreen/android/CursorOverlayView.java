package com.secondscreen.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.view.View;

final class CursorOverlayView extends View {
    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint outlinePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private boolean visible;
    private int cursorX;
    private int cursorY;
    private int streamWidth = 1;
    private int streamHeight = 1;

    CursorOverlayView(Context context) {
        super(context);
        setWillNotDraw(false);
        setClickable(false);
        setFocusable(false);
        fillPaint.setColor(Color.WHITE);
        fillPaint.setStyle(Paint.Style.FILL);
        outlinePaint.setColor(Color.BLACK);
        outlinePaint.setStyle(Paint.Style.STROKE);
        outlinePaint.setStrokeJoin(Paint.Join.ROUND);
    }

    void setStreamSize(int width, int height) {
        streamWidth = Math.max(1, width);
        streamHeight = Math.max(1, height);
        invalidate();
    }

    void updateCursor(CursorState state) {
        visible = state.visible;
        cursorX = state.x;
        cursorY = state.y;
        setStreamSize(state.width, state.height);
        invalidate();
    }

    void clearCursor() {
        visible = false;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (!visible || getWidth() <= 0 || getHeight() <= 0) {
            return;
        }
        float scaleX = getWidth() / (float) streamWidth;
        float scaleY = getHeight() / (float) streamHeight;
        float scale = Math.min(scaleX, scaleY);
        float offsetX = (getWidth() - streamWidth * scale) * 0.5f;
        float offsetY = (getHeight() - streamHeight * scale) * 0.5f;
        float x = offsetX + cursorX * scale;
        float y = offsetY + cursorY * scale;
        float size = Math.max(16f, 28f * scale);

        Path cursor = new Path();
        cursor.moveTo(x, y);
        cursor.lineTo(x, y + size * 1.25f);
        cursor.lineTo(x + size * 0.34f, y + size * 0.98f);
        cursor.lineTo(x + size * 0.58f, y + size * 1.58f);
        cursor.lineTo(x + size * 0.88f, y + size * 1.44f);
        cursor.lineTo(x + size * 0.64f, y + size * 0.88f);
        cursor.lineTo(x + size * 1.08f, y + size * 0.88f);
        cursor.close();
        outlinePaint.setStrokeWidth(Math.max(2f, scale * 2.5f));
        canvas.drawPath(cursor, outlinePaint);
        canvas.drawPath(cursor, fillPaint);
    }

    @Override
    public boolean onTouchEvent(android.view.MotionEvent event) {
        return false;
    }
}
