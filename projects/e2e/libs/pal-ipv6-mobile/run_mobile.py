"""Use the packaged native Net owner and an explicitly reachable IPv6 peer."""
import json
import os
import socket
from fixture_ipv6 import IPv6Fixture, pion

def run_suite(app, args):
    address = os.environ.get('H2_PAL_IPV6_' + args.platform.upper() + '_HOST', 'fec0::2' if args.platform == 'android' else '::1')
    bind = os.environ.get('H2_PAL_IPV6_BIND', '::1')
    callback_bridge = callback_cleanup = None
    if args.platform == 'android':
        def callback_bridge(device_port):
            port = int(app.adb_command('forward', 'tcp:0', 'tcp:' + str(device_port)).stdout.strip())
            return ('127.0.0.1', port)
        def callback_cleanup(port):
            app.adb_command('forward', '--remove', 'tcp:' + str(port))
    with IPv6Fixture(bind=bind, advertise=address, callback_bridge=callback_bridge, callback_cleanup=callback_cleanup) as fixture, pion(args.fixtures["server"], bind, address) as rtc:
        raw = fixture.raw
        dns_host = os.environ.get('H2_PAL_IPV6_DNS_HOST', 'localhost')
        dns_ip = socket.getaddrinfo(dns_host, None, socket.AF_INET6, socket.SOCK_STREAM)[0][4][0]
        if args.platform == 'android' and dns_host == 'localhost':
            import re
            loopback = app.adb_command('shell', 'ip', '-6', 'addr', 'show', 'dev', 'lo').stdout
            match = re.search(r'inet6 ([0-9a-f:]+)/128 scope host', loopback)
            if not match: raise RuntimeError('Android IPv6 loopback unavailable')
            dns_ip = match[1]
            app.environment()['dns_expectation'] = dict(name='localhost', ipv6=dns_ip,
                source='independent DUT ip -6 addr show dev lo; reserved localhost contract')
        settings = dict(host=address, port=raw.port, session=raw.session,
            ca=raw.ca.read_text(), wrong_ca=raw.wrong_ca.read_text(), dns_host=dns_host,
            dns_ip=dns_ip, http_url=fixture.http_url, fallback_url="" if args.platform == "android" else fixture.fallback_url,
            mqtt_port=fixture.mqtt_port, offer=rtc["offer"], stun=rtc["stun"], dns_port=fixture.dns_port)
        if args.platform == 'android':
            settings['observe_android_dns'] = dns_host != 'localhost'
        with app.fixture('fixture.json', json.dumps(settings)):
            result = app.launch(android_log='pal-ipv6-result.json.log')
        fixture.verify(local_fallback=args.platform == "android")
        result['peer'] = fixture.snapshot()
        app.environment()['peer'] = result['peer']
        if args.platform == 'android':
            app.environment()['listener_boundary'] = 'adb control bridge; actual DUT accept peer is IPv6 loopback; routed inbound IPv6 is not qualified'
        result['artifact_sha256'] = app.environment()['app_sha256']
        result['sdk_sha256'] = app.environment()['sdk_sha256']
        result['packaged_sdk_symbols_verified'] = True
        return result

def verify_report(report, args):
    assert report['passed'] == 56
    assert len(report['cases']) == 56 and len({c['id'] for c in report['cases']}) == 56
    assert all(c['status'] == 'PASS' for c in report['cases'])
    assert report['peer']['raw']['session']
