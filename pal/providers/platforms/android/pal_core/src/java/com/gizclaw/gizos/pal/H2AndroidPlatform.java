package com.gizclaw.gizos.pal;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

/**
 * Owns an Android PAL instance and its aspect-fit display/touch View.
 *
 * The host first loads its JNI library containing the PAL (libh2_pal_core.so
 * for AAR consumers), then creates this owner on the UI thread. Mount getView()
 * in any container. The native handle and all borrowed PAL APIs remain valid
 * until close(). Stop/join every Runtime and native caller before closing.
 */
public final class H2AndroidPlatform implements AutoCloseable {
  private final int width;
  private final int height;
  private final Surface view;
  private long handle;

  private static native long nativeCreate(Object view, int width, int height);
  private static native int nativeCopy(long handle, Bitmap frame);
  private static native void nativePointer(long handle, int x, int y, boolean down);
  private static native void nativeDestroy(long handle);

  public H2AndroidPlatform(Context context, int width, int height) {
    requireUiThread();
    if (context == null || width <= 0 || height <= 0)
      throw new IllegalArgumentException("A Context and positive display dimensions are required");
    this.width = width;
    this.height = height;
    view = new Surface(context);
    handle = nativeCreate(view, width, height);
    if (handle == 0) {
      view.frame.recycle();
      throw new IllegalStateException("Could not create the Android PAL");
    }
  }

  /** Borrow the owned View on the UI thread; the caller controls its mounting and layout. */
  public View getView() {
    requireUiThread();
    requireOpen();
    return view;
  }

  /** Borrow h2_android_platform_t for native Runtime assembly. Never free it in the host. */
  public long getNativeHandle() {
    requireUiThread();
    requireOpen();
    return handle;
  }

  /** Release a pending physical press, e.g. when the host becomes inactive. */
  public void cancelPointer() {
    requireUiThread();
    view.releasePointer();
  }

  /** UI-thread only, after every native borrower has stopped. Idempotent. */
  @Override
  public void close() {
    requireUiThread();
    if (handle == 0)
      return;
    view.releasePointer();
    long retiring = handle;
    handle = 0;
    if (view.getParent() instanceof ViewGroup)
      ((ViewGroup) view.getParent()).removeView(view);
    nativeDestroy(retiring);
    view.frame.recycle();
  }

  private static void requireUiThread() {
    if (Looper.myLooper() != Looper.getMainLooper())
      throw new IllegalStateException("Android PAL View operations require the UI thread");
  }
  private void requireOpen() {
    if (handle == 0)
      throw new IllegalStateException("Android PAL is closed");
  }

  private final class Surface extends View {
    private final Bitmap frame = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
    private final Paint paint = new Paint();
    private final RectF content = new RectF();
    private int pointerId = -1;
    Surface(Context context) {
      super(context);
      paint.setFilterBitmap(false);
    }
    /** Called by Display.present() from a native worker; posts onto the UI thread. */
    public void requestFrame() {
      postInvalidateOnAnimation();
    }
    @Override
    protected void onSizeChanged(int w, int h, int oldWidth, int oldHeight) {
      super.onSizeChanged(w, h, oldWidth, oldHeight);
      float scale = Math.min((float) w / width, (float) h / height);
      float left = (w - width * scale) / 2f;
      float top = (h - height * scale) / 2f;
      content.set(left, top, left + width * scale, top + height * scale);
    }
    @Override
    protected void onDraw(Canvas canvas) {
      canvas.drawColor(Color.BLACK);
      if (handle != 0 && nativeCopy(handle, frame) != 0)
        canvas.drawBitmap(frame, null, content, paint);
    }
    private void pointer(MotionEvent event, int index, boolean down) {
      if (handle == 0 || content.width() <= 0 || content.height() <= 0)
        return;
      int x = Math.max(0,
          Math.min(
              width - 1, (int) ((event.getX(index) - content.left) * width / content.width())));
      int y = Math.max(0,
          Math.min(
              height - 1, (int) ((event.getY(index) - content.top) * height / content.height())));
      nativePointer(handle, x, y, down);
    }
    void releasePointer() {
      if (handle != 0)
        nativePointer(handle, 0, 0, false);
      pointerId = -1;
    }
    @Override
    public void onWindowFocusChanged(boolean focused) {
      super.onWindowFocusChanged(focused);
      if (!focused)
        releasePointer();
    }
    @Override
    protected void onDetachedFromWindow() {
      releasePointer();
      super.onDetachedFromWindow();
    }
    @Override
    public boolean onTouchEvent(MotionEvent event) {
      if (handle == 0)
        return false;
      switch (event.getActionMasked()) {
        case MotionEvent.ACTION_DOWN:
          if (!content.contains(event.getX(), event.getY()))
            return false;
          pointerId = event.getPointerId(0);
          pointer(event, 0, true);
          return true;
        case MotionEvent.ACTION_MOVE:
          int index = event.findPointerIndex(pointerId);
          if (index >= 0)
            pointer(event, index, true);
          return true;
        case MotionEvent.ACTION_POINTER_UP:
          if (event.getPointerId(event.getActionIndex()) != pointerId)
            return true;
          pointer(event, event.getActionIndex(), false);
          pointerId = -1;
          return true;
        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_CANCEL:
          pointer(event, 0, false);
          pointerId = -1;
          return true;
        default:
          return true;
      }
    }
  }
}
