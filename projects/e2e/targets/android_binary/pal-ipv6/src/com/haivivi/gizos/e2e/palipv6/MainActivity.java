package com.haivivi.gizos.e2e.palipv6;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.File;
import java.net.InetAddress;
import java.net.Inet6Address;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.JSONObject;

/** Runs the packaged HTTP provider on a worker with owned fixture settings. */
public final class MainActivity extends Activity {
    static { System.loadLibrary("pal_ipv6_e2e"); }
    private static native int nativeRun(String reportPath, String imageVersion,
        String host, int port, String session, String ca, String wrongCa, String dnsHost, String dnsIp, String httpUrl, String fallbackUrl, int mqttPort, String offer, String stun, int dnsPort);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView report = new TextView(this);
        report.setText("PAL IPv6 E2E — running 56 cases…");
        report.setTextIsSelectable(true);
        setContentView(report);
        new Thread(() -> {
            String text;
            try {
                File result = new File(getFilesDir(), "pal-ipv6-result.json");
                JSONObject fixture = new JSONObject(new String(Files.readAllBytes(
                    new File(getFilesDir(), "fixture.json").toPath()), StandardCharsets.UTF_8));
                String observedDns = fixture.getString("dns_ip");
                if (fixture.optBoolean("observe_android_dns")) {
                    observedDns = null;
                    for (InetAddress address : InetAddress.getAllByName(fixture.getString("dns_host"))) {
                        if (address instanceof Inet6Address) {
                            observedDns = address.getHostAddress();
                            break;
                        }
                    }
                    if (observedDns == null) throw new IllegalStateException("AAAA expectation unavailable in the Android resolver view");
                }
                String version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
                int rc = nativeRun(result.getAbsolutePath(), version, fixture.getString("host"), fixture.getInt("port"), fixture.getString("session"),
                    fixture.getString("ca"), fixture.getString("wrong_ca"), fixture.getString("dns_host"), observedDns, fixture.getString("http_url"),
                    fixture.getString("fallback_url"), fixture.getInt("mqtt_port"), fixture.getString("offer"), fixture.getString("stun"), fixture.getInt("dns_port"));
                text = "PAL IPv6 E2E: " + (rc == 0 ? "PASS" : "FAIL (" + rc + ")") + "\n\n"
                    + new String(Files.readAllBytes(result.toPath()), StandardCharsets.UTF_8);
            } catch (Exception error) {
                text = "PAL IPv6 E2E: FAIL\n" + error;
            }
            final String completed = text;
            runOnUiThread(() -> report.setText(completed));
        }, "pal-ipv6-e2e").start();
    }
}
