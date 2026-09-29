package com.haivivi.gizos.e2e.palnettls;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.JSONObject;

/** Runs the packaged HTTP provider on a worker with owned fixture settings. */
public final class MainActivity extends Activity {
    static { System.loadLibrary("pal_net_tls_e2e"); }
    private static native int nativeRun(String reportPath, String imageVersion,
        String host, int port, String session, String ca, String wrongCa);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView report = new TextView(this);
        report.setText("PAL Net/TLS E2E — running 38 cases…");
        report.setTextIsSelectable(true);
        setContentView(report);
        new Thread(() -> {
            String text;
            try {
                File result = new File(getFilesDir(), "pal-net-tls-result.json");
                JSONObject fixture = new JSONObject(new String(Files.readAllBytes(
                    new File(getFilesDir(), "fixture.json").toPath()), StandardCharsets.UTF_8));
                String version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
                int rc = nativeRun(result.getAbsolutePath(), version, fixture.getString("host"), fixture.getInt("port"), fixture.getString("session"),
                    fixture.getString("ca"), fixture.getString("wrong_ca"));
                text = "PAL Net/TLS E2E: " + (rc == 0 ? "PASS" : "FAIL (" + rc + ")") + "\n\n"
                    + new String(Files.readAllBytes(result.toPath()), StandardCharsets.UTF_8);
            } catch (Exception error) {
                text = "PAL Net/TLS E2E: FAIL\n" + error;
            }
            final String completed = text;
            runOnUiThread(() -> report.setText(completed));
        }, "pal-net-tls-e2e").start();
    }
}
