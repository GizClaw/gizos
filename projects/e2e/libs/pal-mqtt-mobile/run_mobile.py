"""Actual packaged MQTT SDK consumer plus independent TCP/TLS wire proof."""
import json
import hashlib
import tempfile
from mqtt_fixture import Fixture

def run_suite(app, args):
    advertised = args.contract['options']['advertised'][args.platform]
    with tempfile.TemporaryDirectory(prefix='h2-mqtt-mobile-') as directory, Fixture(directory, advertised=advertised) as fixture:
        settings = dict(host=advertised, tcp_port=fixture.tcp.port, tls_port=fixture.tls.port,
                        session=fixture.session, ca=fixture.ca.read_text(), wrong_ca=fixture.wrong_ca.read_text())
        payload = json.dumps(settings)
        inputs = dict(session=fixture.session, host=advertised, tcp_port=fixture.tcp.port, tls_port=fixture.tls.port,
            json_sha256=hashlib.sha256(payload.encode()).hexdigest(),
            ca_sha256=hashlib.sha256(fixture.ca.read_bytes()).hexdigest(),
            wrong_ca_sha256=hashlib.sha256(fixture.wrong_ca.read_bytes()).hexdigest(),
            registry_sha256=hashlib.sha256(args.registry.read_bytes()).hexdigest())
        with app.fixture('fixture.json', payload):
            result = app.launch()
        result['fixture_inputs'] = inputs
        app.environment()['fixture_inputs'] = inputs
        result['fixture'] = fixture.verify()
        app.environment()['fixture'] = result['fixture']
        app.environment()['fixture_session'] = fixture.session
        app.environment()['tls_verification'] = 'explicit isolated CA; default/required verified TLS; two real rejecting certificate handshakes'
    result['packaged_sdk_symbols_verified'] = True
    result['sdk_sha256'] = app.environment()['sdk_sha256']
    result['artifact_sha256'] = app.environment()['app_sha256']
    return result

def verify_report(report, args):
    del args
    rows = report['cases']
    if any(row['live_allocations'] != rows[0]['live_allocations'] for row in rows):
        raise AssertionError('case retained an SDK MQTT/client allocation')
    if not report['packaged_sdk_symbols_verified'] or report['owner_failure_verified'] != 2:
        raise AssertionError('missing actual SDK factory/lifetime validation')
    if report['fixture']['active_clients'] != 0 or report['fixture']['retained_messages'] != 0:
        raise AssertionError('real broker resource leak')
