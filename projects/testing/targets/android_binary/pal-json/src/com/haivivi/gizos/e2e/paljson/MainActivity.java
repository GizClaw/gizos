package com.haivivi.gizos.e2e.paljson;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Runs the portable C contract on a worker and displays its unmodified ledger. */
public final class MainActivity extends Activity {
    static { System.loadLibrary("pal_json_e2e"); }
    private static native int nativeRun(String reportPath, String imageVersion);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView report = new TextView(this);
        report.setText("PAL JSON E2E — running mandatory cases…");
        report.setTextIsSelectable(true);
        setContentView(report);
        new Thread(() -> {
            String text;
            try {
                File result = new File(getFilesDir(), "pal-json-result.json");
                String version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
                int rc = nativeRun(result.getAbsolutePath(), version);
                text = "PAL Json E2E: " + (rc == 0 ? "PASS" : "FAIL (" + rc + ")") + "\n\n"
                        + new String(Files.readAllBytes(result.toPath()), StandardCharsets.UTF_8);
            } catch (Exception error) {
                text = "PAL Json E2E: FAIL\n" + error;
            }
            final String completed = text;
            runOnUiThread(() -> report.setText(completed));
        }, "pal-json-e2e").start();
    }
}
