package com.haivivi.firmwares.smokeapps.mp4player;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Bundle;
import android.view.View;
import android.view.Window;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

public final class MainActivity extends Activity {
    private static final int LOGICAL_WIDTH = 1024;
    private static final int LOGICAL_HEIGHT = 600;

    static {
        System.loadLibrary("mp4_player_app");
    }

    private Mp4PlayerView playerView;

    private static native long nativeCreate(
            Mp4PlayerView view, byte[] media, int displayWidth, int displayHeight);
    private static native boolean nativeStart(long handle);
    private static native void nativeRender(long handle, Bitmap bitmap);
    private static native void nativeStop(long handle);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Window window = getWindow();
        window.setStatusBarColor(Color.BLACK);
        window.setNavigationBarColor(Color.BLACK);
        playerView = new Mp4PlayerView();
        setContentView(playerView);
    }

    @Override
    protected void onStart() {
        super.onStart();
        playerView.start();
    }

    @Override
    protected void onStop() {
        if (playerView != null) {
            playerView.close();
        }
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        if (playerView != null) {
            playerView.close();
            playerView = null;
        }
        super.onDestroy();
    }

    private byte[] readMediaAsset() throws IOException {
        try (InputStream input = getAssets().open(
                     "test_1024x600_h264_aac.mp4");
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) != -1) {
                output.write(buffer, 0, count);
            }
            return output.toByteArray();
        }
    }

    public final class Mp4PlayerView extends View {
        private final Bitmap bitmap = Bitmap.createBitmap(
                LOGICAL_WIDTH, LOGICAL_HEIGHT, Bitmap.Config.ARGB_8888);
        private final Paint paint = new Paint();
        private long nativeHandle;

        Mp4PlayerView() {
            super(MainActivity.this);
            setBackgroundColor(Color.BLACK);
            paint.setFilterBitmap(false);
        }

        void start() {
            if (nativeHandle != 0) {
                return;
            }
            try {
                nativeHandle = nativeCreate(
                        this, readMediaAsset(), LOGICAL_WIDTH, LOGICAL_HEIGHT);
            } catch (IOException error) {
                throw new IllegalStateException("Unable to load MP4 asset", error);
            }
            if (nativeHandle == 0 || !nativeStart(nativeHandle)) {
                close();
                throw new IllegalStateException("Unable to start MP4 Player");
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

        void close() {
            if (nativeHandle != 0) {
                nativeStop(nativeHandle);
                nativeHandle = 0;
            }
        }

        private RectF contentRect() {
            float scale = Math.min(
                    (float) getWidth() / LOGICAL_WIDTH,
                    (float) getHeight() / LOGICAL_HEIGHT);
            float scaledWidth = LOGICAL_WIDTH * scale;
            float scaledHeight = LOGICAL_HEIGHT * scale;
            float left = (getWidth() - scaledWidth) * 0.5f;
            float top = (getHeight() - scaledHeight) * 0.5f;
            return new RectF(
                    left, top, left + scaledWidth, top + scaledHeight);
        }
    }
}
