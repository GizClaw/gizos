package com.haivivi.gizos.e2e.palstorage;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

public final class MainActivity extends Activity {
    static { System.loadLibrary("pal_storage_e2e"); }
    private static native int nativeRun(String directory, String report, int phase, long nonce, String version);
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView view = new TextView(this);
        view.setText("PAL Storage E2E running…");
        view.setTextIsSelectable(true);
        setContentView(view);
        int phase = getIntent().getIntExtra("phase", 1);
        long nonce = getIntent().getLongExtra("nonce", 1);
        new Thread(() -> {
            String text;
            try {
                File directory = new File(getFilesDir(), "pal-storage");
                File report = new File(getFilesDir(), "pal-storage-phase.json");
                String version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
                int rc = nativeRun(directory.getAbsolutePath(), report.getAbsolutePath(), phase, nonce, version);
                text = "PAL Storage phase " + phase + ": " + (rc == 0 ? "PASS" : "FAIL (" + rc + ")") + "\n"
                        + new String(Files.readAllBytes(report.toPath()), StandardCharsets.UTF_8);
            } catch (Exception error) { text = "PAL Storage FAIL\n" + error; }
            final String completed = text;
            runOnUiThread(() -> view.setText(completed));
        }, "pal-storage-e2e").start();
    }
}
