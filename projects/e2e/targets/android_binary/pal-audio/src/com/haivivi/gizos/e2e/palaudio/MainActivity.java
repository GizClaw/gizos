package com.haivivi.gizos.e2e.palaudio;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.widget.TextView;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Runs the packaged Android Audio PAL on a worker after mic permission. */
public final class MainActivity extends Activity {
    static { System.loadLibrary("pal_audio_e2e"); }
    private static native int nativeRun(MainActivity view, String reportPath, String version);
    private TextView report;

    // The shared platform constructor requires this UI callback; Audio does
    // not draw and leaves it idle.
    public void requestFrame() {}

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        report = new TextView(this);
        report.setText("PAL Audio E2E — waiting for microphone permission…");
        report.setTextIsSelectable(true);
        setContentView(report);
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED) {
            startAudio();
        } else {
            requestPermissions(new String[] {Manifest.permission.RECORD_AUDIO}, 1);
        }
    }

    @Override public void onRequestPermissionsResult(int requestCode, String[] permissions,
            int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == 1 && grantResults.length == 1 &&
                grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            startAudio();
        } else {
            report.setText("PAL Audio E2E: FAIL — microphone permission denied");
        }
    }

    private void startAudio() {
        report.setText("PAL Audio E2E running…");
        new Thread(() -> {
            String text;
            try {
                File result = new File(getFilesDir(), "pal-audio-result.json");
                String version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
                int rc = nativeRun(this, result.getAbsolutePath(), version);
                text = "PAL Audio E2E: " + (rc == 0 ? "PASS" : "FAIL (" + rc + ")") + "\n\n"
                    + new String(Files.readAllBytes(result.toPath()), StandardCharsets.UTF_8);
            } catch (Exception error) {
                text = "PAL Audio E2E: FAIL\n" + error;
            }
            final String completed = text;
            runOnUiThread(() -> report.setText(completed));
        }, "pal-audio-e2e").start();
    }
}
