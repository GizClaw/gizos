package com.haivivi.firmwares.smokeapps.tapreset;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;

public final class MainActivity extends Activity {
    private TapResetView tapResetView;

    static {
        System.loadLibrary("tap_reset_app");
    }

    private static native long nativeCreate(TapResetView view);
    private static native boolean nativeStart(long handle);
    private static native void nativeRender(long handle, Bitmap bitmap);
    private static native void nativePointer(long handle, int x, int y, boolean pressed);
    private static native void nativeStop(long handle);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Window window = getWindow();
        window.setStatusBarColor(Color.rgb(5, 8, 17));
        window.setNavigationBarColor(Color.rgb(5, 8, 17));
        tapResetView = new TapResetView();
        setContentView(tapResetView);
    }

    @Override
    protected void onDestroy() {
        if (tapResetView != null) {
            tapResetView.close();
            tapResetView = null;
        }
        super.onDestroy();
    }

    public final class TapResetView extends View {
        private static final int LOGICAL_WIDTH = 360;
        private static final int LOGICAL_HEIGHT = 640;

        private final Bitmap bitmap = Bitmap.createBitmap(
                LOGICAL_WIDTH, LOGICAL_HEIGHT, Bitmap.Config.ARGB_8888);
        private final Paint paint = new Paint();
        private long nativeHandle;

        TapResetView() {
            super(MainActivity.this);
            setBackgroundColor(Color.rgb(5, 8, 17));
            paint.setFilterBitmap(false);
            nativeHandle = nativeCreate(this);
            if (nativeHandle == 0 || !nativeStart(nativeHandle)) {
                close();
                throw new IllegalStateException("Unable to start Firmwares portable App");
            }
        }

        public void requestFrame() {
            postInvalidateOnAnimation();
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            if (nativeHandle == 0) {
                return;
            }
            nativeRender(nativeHandle, bitmap);
            canvas.drawBitmap(bitmap, null, contentRect(), paint);
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            if (nativeHandle == 0) {
                return false;
            }
            RectF content = contentRect();
            int x = Math.round((event.getX() - content.left) * LOGICAL_WIDTH
                    / content.width());
            int y = Math.round((event.getY() - content.top) * LOGICAL_HEIGHT
                    / content.height());
            x = Math.max(0, Math.min(LOGICAL_WIDTH - 1, x));
            y = Math.max(0, Math.min(LOGICAL_HEIGHT - 1, y));
            int action = event.getActionMasked();
            boolean pressed = action != MotionEvent.ACTION_UP
                    && action != MotionEvent.ACTION_CANCEL;
            nativePointer(nativeHandle, x, y, pressed);
            return true;
        }

        void close() {
            if (nativeHandle != 0) {
                nativeStop(nativeHandle);
                nativeHandle = 0;
            }
        }

        private RectF contentRect() {
            float scale = Math.min((float) getWidth() / LOGICAL_WIDTH,
                    (float) getHeight() / LOGICAL_HEIGHT);
            float width = LOGICAL_WIDTH * scale;
            float height = LOGICAL_HEIGHT * scale;
            float left = (getWidth() - width) * 0.5f;
            float top = (getHeight() - height) * 0.5f;
            return new RectF(left, top, left + width, top + height);
        }
    }
}
