package com.haivivi.gizos.e2e.atomic;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/** Runs the portable C contract on a worker and displays its unmodified ledger. */
public final class MainActivity extends Activity {
    static { System.loadLibrary("atomic_e2e"); }
    private static native int nativeRun(String reportPath, String imageVersion);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView report = new TextView(this);
        report.setText("Atomic E2E — running mandatory cases…");
        report.setTextIsSelectable(true);
        setContentView(report);
        new Thread(() -> {
            String text;
            try {
                File result = new File(getFilesDir(), "atomic-result.json");
                String version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
                int rc = nativeRun(result.getAbsolutePath(), version);
                text = "Atomic E2E: " + (rc == 0 ? "PASS" : "FAIL (" + rc + ")") + "\n\n"
                        + new String(Files.readAllBytes(result.toPath()), StandardCharsets.UTF_8);
            } catch (Exception error) {
                text = "Atomic E2E: FAIL\n" + error;
            }
            final String completed = text;
            runOnUiThread(() -> report.setText(completed));
        }, "atomic-e2e").start();
    }
}
