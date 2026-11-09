package com.haivivi.gizos.e2e.palhttp;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.JSONObject;

/** Runs the packaged HTTP provider on a worker with owned fixture settings. */
public final class MainActivity extends Activity {
    static { System.loadLibrary("pal_http_e2e"); }
    private static native int nativeRun(String reportPath, String imageVersion,
        String http, String https, String untrusted, String ca);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView report = new TextView(this);
        report.setText("PAL HTTP E2E — running 45 cases…");
        report.setTextIsSelectable(true);
        setContentView(report);
        new Thread(() -> {
            String text;
            try {
                File result = new File(getFilesDir(), "pal-http-result.json");
                JSONObject fixture = new JSONObject(new String(Files.readAllBytes(
                    new File(getFilesDir(), "fixture.json").toPath()), StandardCharsets.UTF_8));
                String version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
                int rc = nativeRun(result.getAbsolutePath(), version, fixture.getString("http"),
                    fixture.getString("https"), fixture.getString("untrusted"), fixture.getString("ca"));
                text = "PAL HTTP E2E: " + (rc == 0 ? "PASS" : "FAIL (" + rc + ")") + "\n\n"
                    + new String(Files.readAllBytes(result.toPath()), StandardCharsets.UTF_8);
            } catch (Exception error) {
                text = "PAL HTTP E2E: FAIL\n" + error;
            }
            final String completed = text;
            runOnUiThread(() -> report.setText(completed));
        }, "pal-http-e2e").start();
    }
}
