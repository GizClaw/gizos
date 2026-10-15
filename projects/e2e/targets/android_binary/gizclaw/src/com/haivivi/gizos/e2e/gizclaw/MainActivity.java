package com.haivivi.gizos.e2e.gizclaw;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONObject;

public final class MainActivity extends Activity {
    static { System.loadLibrary("gizclaw_e2e"); }
    private static final AtomicBoolean started = new AtomicBoolean();
    private static native int nativeRun(String directory, String version,
        String endpoint, String token, String api, String audio, byte[] pcm);
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView report = new TextView(this);
        report.setText("GizClaw E2E running…");
        report.setTextIsSelectable(true);
        setContentView(report);
        if (!started.compareAndSet(false, true)) return;
        new Thread(() -> {
            String text;
            try {
                JSONObject fixture = new JSONObject(new String(Files.readAllBytes(
                    new File(getFilesDir(), "fixture.json").toPath()), StandardCharsets.UTF_8));
                byte[] pcm = Files.readAllBytes(new File(getFilesDir(), "voice.pcm").toPath());
                String version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
                int rc = nativeRun(getFilesDir().getAbsolutePath(), version,
                    fixture.getString("endpoint"), fixture.getString("token"),
                    fixture.getString("api_url"), fixture.getString("audio_url"), pcm);
                text = "GizClaw E2E: " + (rc == 0 ? "PASS" : "FAIL (" + rc + ")") + "\n"
                    + new String(Files.readAllBytes(new File(getFilesDir(), "gizclaw-result.json").toPath()), StandardCharsets.UTF_8);
            } catch (Exception error) {
                text = "GizClaw E2E: fixture or launcher failure (" + error.getClass().getSimpleName() + ")";
            }
            final String completed = text;
            runOnUiThread(() -> report.setText(completed));
        }, "gizclaw-e2e").start();
    }
}
