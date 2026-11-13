package com.gizclaw.fruitslice;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;
import android.widget.Toast;
import com.gizclaw.gizos.pal.H2AndroidPlatform;
import java.io.File;

public final class MainActivity extends Activity {
  static {
    System.loadLibrary("fruit_slice");
  }
  private static native long nativeCreate(long platform, String directory, String version);
  private static native int nativeRun(long owner);
  private static native void nativeState(long owner, boolean paused, boolean stopped);
  private static native int nativeDestroy(long owner);
  private H2AndroidPlatform platform;
  private long owner; // UI-thread owned; the worker captures a stable handle.

  @Override
  public void onCreate(Bundle state) {
    super.onCreate(state);
    try {
      platform = new H2AndroidPlatform(this, 480, 800);
      platform.getView().setKeepScreenOn(true);
      setContentView(platform.getView());
      String version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
      String directory = new File(getFilesDir(), "fruit_slice").getAbsolutePath();
      owner = nativeCreate(platform.getNativeHandle(), directory, version);
    } catch (Exception error) {
      Log.e("FruitSlice", "Initialization failed", error);
    }
    if (owner == 0) {
      if (platform != null) {
        platform.close();
        platform = null;
      }
      TextView message = new TextView(this);
      message.setText("Fruit Slice could not initialize its native platform.");
      setContentView(message);
      return;
    }
    final H2AndroidPlatform current = platform;
    final long session = owner;
    new Thread(() -> {
      int result = nativeRun(session);
      runOnUiThread(() -> {
        int destroyed = nativeDestroy(session);
        if (destroyed == 0) {
          owner = 0;
          current.close(); // Runtime has joined; no native caller can still borrow the View.
          platform = null;
        } else {
          Log.e("FruitSlice", "Runtime still busy; retaining platform: " + destroyed);
        }
        if (result != 0 && !isFinishing() && !isDestroyed())
          Toast.makeText(this, "Game stopped (" + result + ")", Toast.LENGTH_LONG).show();
      });
    }, "fruit-slice/game").start();
  }
  @Override
  protected void onResume() {
    super.onResume();
    if (owner != 0)
      nativeState(owner, false, false);
  }
  @Override
  protected void onPause() {
    if (platform != null)
      platform.cancelPointer();
    if (owner != 0)
      nativeState(owner, true, false);
    super.onPause();
  }
  @Override
  protected void onDestroy() {
    if (owner != 0)
      nativeState(owner, true, true);
    super.onDestroy();
  }
}
