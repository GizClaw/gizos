"""Record only IPv6 source/audit maintenance; retain every physical receipt."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

BASE = "e9ee7e6b4d2a15b70bb3a7a9147bacf3937365c3"
ROOT = Path(__file__).resolve().parents[4]
NET = Path(__file__).resolve().parent
DISPLAY = ROOT / "projects/e2e/apps/pal-display"
SOURCES = {
    "projects/e2e/apps/pal-net-tls/app/src/h2_pal_net_tls_e2e.c",
    "projects/e2e/apps/pal-net-tls/app/include/h2_pal_net_tls_e2e.h",
    "projects/e2e/libs/pal-net-tls-fixture/fixture.py",
    "libs/pal/include/h2/pal/net/h2_pal_net.h",
    "libs/pal/providers/posix/pal_core/src/h2_posix_net.c",
    "native_component_src/esp-idf6.x/h2_pal_core/src/h2_esp_platform_net.c",
}
BK = "native_component_src/bk7258/ap/h2_pal_core/src/h2_bk_platform_net.c"
BK_WIFI = "native_component_src/bk7258/ap/h2_pal_core/src/h2_bk_platform_wifi.c"
GUIDE = "guides/zh/developing/platform_abstract_layer.md"


def digest(value):
    return hashlib.sha256(value).hexdigest()


def baseline(path):
    return subprocess.check_output(["git", "show", BASE + ":" + path], cwd=ROOT)


def summary(path, prefix):
    rows = [json.loads(line[len(prefix):]) for line in path.read_text().splitlines()
            if line.startswith(prefix)]
    assert len(rows) == 1
    return rows[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--validation-dir", type=Path, required=True)
    args = parser.parse_args()
    v6log = args.validation_dir / "desktop_pal_ipv6_test.raw.log"
    v4log = args.validation_dir / "desktop_pal_net_tls_test.raw.log"
    rejected = args.validation_dir / "desktop_pal_net_tls_endpoint_rejection_test.raw.log"
    host = json.loads((args.validation_dir / "host_source_manifest.json").read_text())
    assert host["schema"] == 1 and host["platform"] == "macos"
    for path, expected in host["source_sha256"].items():
        assert digest((ROOT / path).read_bytes()) == expected, "host closure changed; capture a fresh run: " + path
    v6 = summary(v6log, "H2_PAL_IPV6_SUMMARY ")
    v4 = summary(v4log, "H2_PAL_NET_TLS_SUMMARY ")
    negative = summary(rejected, "H2_PAL_NET_TLS_SUMMARY ")
    assert v6["passed"] == 56 and len(v6["cases"]) == len({c["id"] for c in v6["cases"]}) == 56
    assert v4["mandatory_passed"] == 37 and negative["core_qualified"] is False and negative["failed"] == 1
    for value in (v4, v6):
        assert all(value[name] == 0 for name in ("failed", "blocked", "rc", "teardown",
                     "retained_sockets", "retained_resolvers", "retained_allocations"))
    historical = json.loads((NET / "qualification.json").read_text())
    https = json.loads((NET / "gizclaw_public_https_provenance_main_tls.json").read_text())
    prior = {**historical["source_sha256"], **https["current_source_sha256"]}
    sources = set(SOURCES)
    for path in (BK, BK_WIFI):
        if digest((ROOT / path).read_bytes()) != prior[path]:
            sources.add(path)
    validation = {
        "desktop_ipv6_mandatory_passed": 56, "desktop_net_tls_mandatory_passed": 37,
        "retained_resources": 0, "endpoint_rejection": "EXPECTED_REJECTION",
        "new_physical_run_claimed": False,
        "host_execution": host,
        "raw_log_sha256": {path.name: digest(path.read_bytes()) for path in (v6log, v4log, rejected)}}
    net_record = {"schema": 1, "new_physical_run_claimed": False,
        "historical_physical_qualification_applies_to_current_sources": False,
        "historical_qualification_sha256": digest((NET / "qualification.json").read_bytes()),
        "historical_https_provenance_sha256": digest((NET / "gizclaw_public_https_provenance_main_tls.json").read_bytes()),
        "previous_source_sha256": {p: prior[p] for p in sorted(sources)},
        "current_source_sha256": {p: digest((ROOT / p).read_bytes()) for p in sorted(sources)},
        "new_slots": {"resolve_all": ["dns-ipv4-filter", "dns-ipv6-filter", "dns-any-families"],
            "resolve_start_family": ["dns-family-async-copy", "dns-family-cancel"],
            "resolve_poll_all": ["dns-family-async-copy"],
            "get_host_addr_family": ["netif-ipv6-address"]},
        "validation": validation,
        "scope": "Current IPv6 parameterization/provider source maintenance and fresh host regression only. Historical device, mobile, browser, source, image and artifact receipts remain byte-for-byte unchanged. Historical 21-operation receipts do not qualify the four additive IPv6 operations or current physical provider images."}
    audit = json.loads((DISPLAY / "shared_catalog_provenance.json").read_text())
    old = baseline(GUIDE).decode()
    current = (ROOT / GUIDE).read_text()
    changes = []
    for tag, heading, end in [("net_ipv6", "\n## IPv6 地址与 DNS\n", None),
                              ("wifi_ipv6_readiness", "## Wi-Fi IPv6 就绪\n", "## Wi-Fi 连接与持久化\n")]:
        if heading in current and heading not in old:
            start = current.index(heading)
            finish = len(current) if end is None else current.index(end, start)
            addition = current[start:finish]
            changes.append({"owner": tag, "before_utf8": "", "after_utf8": addition})
            current = current[:start] + current[finish:]
    prefix = "Wi-Fi STA 的 `connect` 与 `connect_and_save`"
    old_para = next(p for p in old.split("\n\n") if p.startswith(prefix))
    new_para = next(p for p in current.split("\n\n") if p.startswith(prefix))
    if old_para != new_para:
        changes.append({"owner": "wifi_ipv6_persistence", "before_utf8": old_para, "after_utf8": new_para})
        current = current.replace(new_para, old_para, 1)
    assert current == old, "unowned PAL guide change cannot be admitted"
    display_record = {"schema": 1, "new_physical_run_claimed": False,
        "historical_shared_catalog_provenance_sha256": digest((DISPLAY / "shared_catalog_provenance.json").read_bytes()),
        "historical_baseline_commit": BASE, "historical_baseline_sha256": digest(baseline(GUIDE)),
        "guide_changes": changes,
        "previous_audit_sha256": audit["current_source_sha256"],
        "current_audit_sha256": {p: digest((ROOT / p).read_bytes()) for p in sorted(audit["current_source_sha256"])},
        "network_config_changes": {},
        "scope": "Host audit and explicitly reversed Net/Wi-Fi guide deltas only. Display provider/App/catalog content and historical physical/mobile/artifact receipts are unchanged; no current Display hardware qualification is claimed."}
    cfg = "boards/bk7258_v3_202405/bk7258/ap.defaults"
    before, after = baseline(cfg).decode(), (ROOT / cfg).read_text()
    if before != after:
        matcher = difflib.SequenceMatcher(None, before.splitlines(keepends=True), after.splitlines(keepends=True))
        hunks = [(i, j, k, l) for op, i, j, k, l in matcher.get_opcodes() if op != "equal"]
        assert len(hunks) == 1, "only one exact IPv6 network-default hunk is admitted"
        i, j, k, l = hunks[0]
        previous = "".join(before.splitlines(keepends=True)[i:j])
        updated = "".join(after.splitlines(keepends=True)[k:l])
        assert previous == "# CONFIG_IPV6 is not set\n"
        assert updated == "CONFIG_IPV6=y\n"
        display_record["network_config_changes"][cfg] = {"previous_sha256": digest(before.encode()),
            "current_sha256": digest(after.encode()), "before_utf8": previous, "after_utf8": updated}
    for path, value in [(NET / "ipv6_source_maintenance.json", net_record),
                        (DISPLAY / "shared_ipv6_maintenance.json", display_record)]:
        path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
