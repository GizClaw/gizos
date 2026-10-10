"""Reproduce host audit inputs; never rewrite any qualification receipt."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import check_qualification as qualification


def sha256(content):
    return hashlib.sha256(content).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    root = qualification.ROOT
    anchor = qualification.SHARED_CATALOG_BASELINE_COMMIT
    path = qualification.SHARED_CATALOG
    baseline = subprocess.check_output(["git", "show", f"{anchor}:{path}"])
    maintenance_bytes = (root / "gizclaw_harness_provenance.json").read_bytes()
    maintenance = json.loads(maintenance_bytes)
    historical = json.loads((root / "qualification.json").read_bytes())
    mobile = json.loads((root / "mobile_runner_refactor.json").read_bytes())
    previous = {**historical["source_sha256"],
                **historical["review_fix_source_sha256"],
                **mobile["current_source_sha256"], **mobile["audit_source_sha256"],
                **maintenance["current_source_sha256"]}
    assert sha256(baseline) == previous[path], "historical catalog identity changed"
    assert qualification.display_catalog_content(baseline.decode("utf-8")) == (
        qualification.display_catalog_content(Path(path).read_text(encoding="utf-8")))
    pal_path = qualification.SHARED_PAL_GUIDE
    pal_baseline = subprocess.check_output(["git", "show", f"{anchor}:{pal_path}"])
    assert sha256(pal_baseline) == previous[pal_path]
    start, end = qualification.modem_guide_section(pal_baseline)
    modem_section = pal_baseline[start:end]
    assert sha256(modem_section) == qualification.SHARED_PAL_MODEM_BASELINE_SHA256
    modem_record = (json.dumps({"source_commit": anchor, "source_path": pal_path,
                               "section_utf8": modem_section.decode("utf-8")},
                              ensure_ascii=False, indent=2) + "\n").encode()
    modem_path = root / "shared_pal_modem_baseline.json"
    if args.apply:
        modem_path.write_bytes(modem_record)
    else:
        assert modem_path.read_bytes() == modem_record
    marker = b"ESP LittleFS "
    assert pal_baseline.count(marker) == 1
    offset = pal_baseline.index(marker)
    assert offset == qualification.SHARED_PAL_ADDITION_OFFSET
    addition_anchor = qualification.SHARED_PAL_ADDITION_COMMIT
    extended = subprocess.check_output(["git", "show", f"{addition_anchor}:{pal_path}"])
    suffix = pal_baseline[offset:]
    assert extended.startswith(pal_baseline[:offset]) and extended.endswith(suffix)
    addition = extended[offset:len(extended) - len(suffix)]
    assert sha256(addition) == qualification.SHARED_PAL_ADDITION_SHA256
    assert extended == pal_baseline[:offset] + addition + suffix
    pal_current = qualification.historical_modem_guide(Path(pal_path).read_bytes())
    assert pal_current in (pal_baseline, pal_baseline[:offset] + addition + pal_baseline[offset:])
    qualification.retired_pal_makefile_source(Path("Makefile").read_bytes(), previous["Makefile"])
    web_display = "libs/pal/providers/web/pal_core/src/h2_web_platform_display.c"
    qualification.web_display_output_source(Path(web_display).read_bytes(), previous[web_display])
    sources = sorted(qualification.SHARED_CATALOG_AUDIT_SOURCES)
    value = {
        "schema": 1,
        "new_physical_run_claimed": False,
        "historical_harness_receipt_sha256": sha256(maintenance_bytes),
        "catalog_baseline": {"source_commit": anchor, "source_path": path,
                             "source_sha256": sha256(baseline)},
        "pal_guide_extension": {"source_commit": anchor, "source_path": pal_path,
                                "source_sha256": sha256(pal_baseline),
                                "addition_source_commit": addition_anchor,
                                "insertion_offset": offset, "addition_sha256": sha256(addition)},
        "pal_modem_scope": {"source_commit": anchor, "source_path": pal_path,
                            "baseline_sha256": qualification.SHARED_PAL_MODEM_BASELINE_SHA256,
                            "scope": "Only Modem volume and emergency/OTA sections; no Display or general PAL policy changes"},
        "previous_source_sha256": {item: previous[item] for item in sources},
        "current_source_sha256": {item: sha256(Path(item).read_bytes()) for item in sources},
        "scope": "Host audit maintenance only; exact Display catalog section and row remain historical. The shared PAL guide admits only the exact two BK Pref paragraphs and independent Modem-owned sections projected onto their immutable baseline. Makefile admits only removal of the legacy PAL MQTT phony token and forwarding recipe: reinserting those exact bytes must match its historical whole-file digest. Web display admits only its two exact host backlight snapshot assignments; removing them must recover the historical provider digest. All other guide bytes, qualification, mobile execution, artifact and hardware identities are unchanged.",
    }
    encoded = (json.dumps(value, indent=2) + "\n").encode()
    baseline_path = root / "shared_catalog_baseline.txt"
    addition_path = root / "shared_pal_pref_addition.json"
    addition_record = (json.dumps({"source_commit": addition_anchor, "source_path": pal_path,
                                  "addition_utf8": addition.decode("utf-8")},
                                 ensure_ascii=False, indent=2) + "\n").encode()
    provenance_path = root / "shared_catalog_provenance.json"
    if args.apply:
        baseline_path.write_bytes(baseline)
        addition_path.write_bytes(addition_record)
        provenance_path.write_bytes(encoded)
    else:
        assert baseline_path.read_bytes() == baseline
        assert addition_path.read_bytes() == addition_record
        assert provenance_path.read_bytes() == encoded
    print("PASS immutable catalog baseline and separate host audit provenance")


if __name__ == "__main__":
    main()
