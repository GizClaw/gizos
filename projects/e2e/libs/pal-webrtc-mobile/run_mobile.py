"""Own the Pion peer lifecycle and precise fingerprint rejection evidence."""
import json
import os

from fixture import fixture


def run_suite(app, args):
    address = args.contract["options"]["ios_address"] if args.platform == "ios" else os.environ.get(args.contract["options"]["address_env"])
    if not address:
        raise ValueError("Android requires explicit host LAN H2_WEBRTC_FIXTURE_IP for UDP ICE")
    with fixture(args.fixtures["server"], address) as settings:
        with app.fixture("fixture.json", json.dumps(settings)):
            result = app.launch()
        app.environment()["fixture"] = "isolated Pion; real UDP ICE/DTLS/SRTP/SCTP"
    return result


def verify_report(report, args):
    expected = args.contract["options"]
    authentication = next(case for case in report["cases"] if case["id"] == expected["authentication_case"])
    assert authentication.get("observed_error") == expected["authentication_error"] and authentication.get("authentication_evidence") == expected["authentication_evidence"], "missing precise native fingerprint authentication failure"
