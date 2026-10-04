package com.haivivi.gizos.e2e.palmqtt;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.JSONObject;

/** Runs the packaged MQTT provider on a worker with owned fixture settings. */
public final class MainActivity extends Activity {
    static { System.loadLibrary("pal_mqtt_e2e"); }
    private static native int nativeRun(String reportPath, String imageVersion,
        String host, int tcpPort, int tlsPort, String session, String ca, String wrongCA);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView report = new TextView(this);
        report.setText("PAL MQTT E2E — running 36 cases…");
        report.setTextIsSelectable(true);
        setContentView(report);
        new Thread(() -> {
            String text;
            try {
                File result = new File(getFilesDir(), "pal-mqtt-result.json");
                JSONObject fixture = new JSONObject(new String(Files.readAllBytes(
                    new File(getFilesDir(), "fixture.json").toPath()), StandardCharsets.UTF_8));
                String version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
                int rc = nativeRun(result.getAbsolutePath(), version, fixture.getString("host"),
                    fixture.getInt("tcp_port"), fixture.getInt("tls_port"), fixture.getString("session"),
                    fixture.getString("ca"), fixture.getString("wrong_ca"));
                text = "PAL MQTT E2E: " + (rc == 0 ? "PASS" : "FAIL (" + rc + ")") + "\n\n"
                    + new String(Files.readAllBytes(result.toPath()), StandardCharsets.UTF_8);
            } catch (Exception error) {
                text = "PAL MQTT E2E: FAIL\n" + error;
            }
            final String completed = text;
            runOnUiThread(() -> report.setText(completed));
        }, "pal-mqtt-e2e").start();
    }
}
