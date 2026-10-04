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
    sources = sorted(qualification.SHARED_CATALOG_AUDIT_SOURCES)
    value = {
        "schema": 1,
        "new_physical_run_claimed": False,
        "historical_harness_receipt_sha256": sha256(maintenance_bytes),
        "catalog_baseline": {"source_commit": anchor, "source_path": path,
                             "source_sha256": sha256(baseline)},
        "previous_source_sha256": {item: previous[item] for item in sources},
        "current_source_sha256": {item: sha256(Path(item).read_bytes()) for item in sources},
        "scope": "Host audit maintenance only; exact Display catalog section and row remain historical. All qualification, mobile execution, artifact and hardware identities are unchanged.",
    }
    encoded = (json.dumps(value, indent=2) + "\n").encode()
    baseline_path = root / "shared_catalog_baseline.txt"
    provenance_path = root / "shared_catalog_provenance.json"
    if args.apply:
        baseline_path.write_bytes(baseline)
        provenance_path.write_bytes(encoded)
    else:
        assert baseline_path.read_bytes() == baseline
        assert provenance_path.read_bytes() == encoded
    print("PASS immutable catalog baseline and separate host audit provenance")


if __name__ == "__main__":
    main()
