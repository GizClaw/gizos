package com.haivivi.gizos.e2e.palwebrtc;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.JSONObject;

/** Runs the packaged WebRTC provider on a worker with owned fixture settings. */
public final class MainActivity extends Activity {
    static { System.loadLibrary("pal_webrtc_e2e"); }
    private static native int nativeRun(String reportPath, String imageVersion,
        String offer, String stun);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView report = new TextView(this);
        report.setText("PAL WebRTC E2E — running contract cases…");
        report.setTextIsSelectable(true);
        setContentView(report);
        new Thread(() -> {
            String text;
            try {
                File result = new File(getFilesDir(), "pal-webrtc-result.json");
                JSONObject fixture = new JSONObject(new String(Files.readAllBytes(
                    new File(getFilesDir(), "fixture.json").toPath()), StandardCharsets.UTF_8));
                String version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
                int rc = nativeRun(result.getAbsolutePath(), version, fixture.getString("offer"), fixture.getString("stun"));
                text = "PAL WebRTC E2E: " + (rc == 0 ? "PASS" : "FAIL (" + rc + ")") + "\n\n"
                    + new String(Files.readAllBytes(result.toPath()), StandardCharsets.UTF_8);
            } catch (Exception error) {
                text = "PAL WebRTC E2E: FAIL\n" + error;
            }
            final String completed = text;
            runOnUiThread(() -> report.setText(completed));
        }, "pal-webrtc-e2e").start();
    }
}
