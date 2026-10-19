"""Parse the native Display line ledger; lifecycle and probes are shared."""
import json


def parse_report(raw):
    lines = raw.splitlines()
    reports = [json.loads(line[18:]) for line in lines if line.startswith("H2_DISPLAY_REPORT ")]
    if not reports:
        raise ValueError("incomplete Display report")
    report = reports[-1]
    report["cases"] = [json.loads(line[16:]) for line in lines if line.startswith("H2_DISPLAY_CASE ")]
    return report
