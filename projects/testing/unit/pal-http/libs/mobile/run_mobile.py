"""Own the real HTTP/TLS fixture and its peer-side rejection evidence."""
import json
import tempfile

from fixture import Fixture


def run_suite(app, args):
    advertised = args.contract["options"]["advertised"][args.platform]
    with tempfile.TemporaryDirectory(prefix=app.prefix + "-fixture-") as directory, Fixture(directory, advertised=advertised) as fixture:
        settings = dict(http=fixture.http, https=fixture.https, untrusted=fixture.untrusted,
                        ca=fixture.ca.read_text())
        with app.fixture("fixture.json", json.dumps(settings)):
            result = app.launch()
        app.environment()["fixture_attempts"] = fixture.verify_arrivals()
        app.environment()["tls_rejection"] = fixture.verify_tls_rejection()
        app.environment()["tls_verification"] = "required; explicit isolated test CA; separate untrusted certificate rejected"
    return result
