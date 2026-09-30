"""Own only the raw Net/TLS fixture and exact peer-side qualification proof."""
import json
import socket

from fixture import Fixture
from check_qualification import check_cases, check_peer


def run_suite(app, args):
    if args.platform == 'android':
        def callback_bridge(device_port):
            port = int(app.adb_command('forward', 'tcp:0', 'tcp:' + str(device_port)).stdout.strip())
            assert 0 < port <= 65535
            return port

        def callback_cleanup(port):
            app.adb_command('forward', '--remove', 'tcp:' + str(port))
    else:
        callback_bridge = callback_cleanup = None
    options = args.contract['options']
    with Fixture(advertise=options['advertised'][args.platform],
                 callback_bridge=callback_bridge, callback_cleanup=callback_cleanup) as fixture:
        dns_host = options['dns_host']
        dns_ip = socket.getaddrinfo(dns_host, None, socket.AF_INET, socket.SOCK_STREAM)[0][4][0]
        settings = dict(dns_host=dns_host, dns_ip=dns_ip, host=fixture.advertise,
                        port=fixture.port, session=fixture.session,
                        ca=fixture.ca.read_text(), wrong_ca=fixture.wrong_ca.read_text())
        with app.fixture('fixture.json', json.dumps(settings)):
            result = app.launch(android_log='pal-net-tls-result.json.log')
        result['peer'] = fixture.snapshot()
        result['dns'] = dict(host=dns_host, operator_ipv4=dns_ip)
        app.environment()['peer'] = result['peer']
        app.environment()['dns'] = result['dns']
        app.environment()['tls_verification'] = (
            'required/default explicit isolated test CA; typed verification rejection with exact peer proof')
    result['packaged_sdk_symbols_verified'] = True
    result['sdk_exported_symbols'] = ['h2_' + args.platform + '_net_' + name
                                      for name in ('create', 'api', 'destroy')]
    result['sdk_sha256'] = app.environment()['sdk_sha256']
    result['artifact_sha256'] = app.environment()['app_sha256']
    return result


def verify_report(report, args):
    # Require the portable mandatory flags as well as the declarative status checks.
    import re
    registry = re.findall(r'H2_NET_TLS_CASE\(\w+, "([^"]+)", ([01])\)', args.registry.read_text())
    check_cases(report, registry)
    check_peer(report['peer'], report['peer']['session'], report['peer']['session'][:16])
