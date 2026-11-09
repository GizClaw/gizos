package com.haivivi.gizos.e2e.paldisplay;
import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.Bundle;
import android.view.View;
import java.io.File;
import java.io.FileOutputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class MainActivity extends Activity {
  static {
    System.loadLibrary("pal_display_e2e");
  }
  private static native long nativeCreate(Object view);
  private static native int nativeCopy(long owner, Bitmap bitmap);
  private static native int nativeRun(long owner, Object view, String report);
  @Override
  public void onCreate(Bundle state) {
    super.onCreate(state);
    Surface surface = new Surface();
    setContentView(surface);
    surface.owner = nativeCreate(surface);
    new Thread(() -> {
      File report = new File(getFilesDir(), "pal-display-result.log");
      nativeRun(surface.owner, surface, report.getAbsolutePath());
      surface.owner = 0;
    }, "pal-display-e2e").start();
  }
  public final class Surface extends View {
    volatile long owner;
    private final Bitmap frame = Bitmap.createBitmap(96, 80, Bitmap.Config.ARGB_8888);
    private final Paint paint = new Paint();
    Surface() {
      super(MainActivity.this);
      setLayerType(View.LAYER_TYPE_SOFTWARE, null);
    }
    public void retire() {
      CountDownLatch done = new CountDownLatch(1);
      post(() -> {
        owner = 0;
        done.countDown();
      });
      try {
        if (!done.await(20, TimeUnit.SECONDS))
          throw new IllegalStateException("retire timeout");
      } catch (InterruptedException error) {
        Thread.currentThread().interrupt();
        throw new IllegalStateException(error);
      }
    }
    public void requestFrame() {
      postInvalidate();
    }
    @Override
    protected void onDraw(Canvas canvas) {
      canvas.drawColor(Color.BLACK);
      if (owner != 0 && nativeCopy(owner, frame) != 0)
        canvas.drawBitmap(frame, 0, 0, paint);
    }
    /**
     * Capture View.draw's completed Canvas output, after the actual provider
     * bitmap reached the real View. No native shadow/input-buffer inspection.
     */
    public int capture(int[] expected, int brightness, String id) {
      CountDownLatch done = new CountDownLatch(1);
      int[] result = {-4};
      post(() -> {
        Bitmap rendered = Bitmap.createBitmap(96, 80, Bitmap.Config.ARGB_8888);
        try {
          draw(new Canvas(rendered));
          int[] actual = new int[96 * 80];
          rendered.getPixels(actual, 0, 96, 0, 0, 96, 80);
          result[0] = 0;
          for (int i = 0; i < actual.length; ++i) {
            int p = expected[i];
            int[] want = {
                ((p >> 11) & 31) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31};
            int[] got = {Color.red(actual[i]), Color.green(actual[i]), Color.blue(actual[i])};
            for (int c = 0; c < 3; ++c)
              if (Math.abs(got[c] - want[c] * brightness / 100) > 3)
                result[0] = -4;
          }
          try (FileOutputStream output =
                   new FileOutputStream(new File(getFilesDir(), id + "-" + brightness + ".png"))) {
            if (!rendered.compress(Bitmap.CompressFormat.PNG, 100, output))
              result[0] = -4;
          }
        } catch (Exception error) {
          result[0] = -4;
        } finally {
          rendered.recycle();
          done.countDown();
        }
      });
      try {
        if (!done.await(20, TimeUnit.SECONDS))
          return -6;
      } catch (InterruptedException error) {
        Thread.currentThread().interrupt();
        return -6;
      }
      return result[0];
    }
  }
}
