import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import socket
import os
from fixture import Fixture

parser = argparse.ArgumentParser()
parser.add_argument('--binary', required=True)
parser.add_argument('--negative-endpoints', action='store_true')
args = parser.parse_args()
dns_host = os.environ.get('H2_PAL_NET_TLS_DNS_HOST', 'ap.e2e.gizclaw.com')
dns_ip = socket.getaddrinfo(dns_host, None, socket.AF_INET, socket.SOCK_STREAM)[0][4][0]
with Fixture() as fixture:
    if args.negative_endpoints:
        original = fixture.arm
        def damaged(case, mode, callback, peer, run_id=None):
            port = original(case, mode, callback, peer, run_id=run_id)
            if case == 'tls-wrong-ca':
                for sock in list(fixture.sockets):
                    try:
                        if sock.getsockname()[1] == port:
                            sock.close()
                    except OSError:
                        pass
            return port
        fixture.arm = damaged
    run = subprocess.run([str(Path(args.binary).resolve()), fixture.advertise,
        str(fixture.port), fixture.session, str(fixture.ca), str(fixture.wrong_ca), dns_host, dns_ip],
        capture_output=True, text=True, timeout=180)
    print(run.stdout, end='')
    print(run.stderr, end='')
    cases = [json.loads(line.split(' ', 1)[1]) for line in run.stdout.splitlines()
             if line.startswith('H2_PAL_NET_TLS_CASE ')]
    summaries = [json.loads(line.split(' ', 1)[1]) for line in run.stdout.splitlines()
                 if line.startswith('H2_PAL_NET_TLS_SUMMARY ')]
    if args.negative_endpoints:
        wrong = next((item for item in cases if item['id'] == 'tls-wrong-ca'), {})
        assert run.returncode != 0 and wrong.get('status') == 'FAIL', wrong
    else:
        assert run.returncode == 0, run.returncode
        assert len(cases) == 39 and len({item['id'] for item in cases}) == 39
        assert len(summaries) == 1 and summaries[0]['core_qualified'] is True
        assert summaries[0]['mandatory_passed'] == 37
        assert not summaries[0]['full_net_qualified']
        assert all(item['status'] == 'PASS' for item in cases if item['mandatory'])
        assert not any(summaries[0][key] for key in ('failed', 'blocked', 'retained_sockets', 'retained_resolvers', 'retained_allocations', 'rc', 'teardown'))
        receipt = dict(platform='macos' if __import__('sys').platform=='darwin' else 'linux',
            artifact_sha256=hashlib.sha256(Path(args.binary).read_bytes()).hexdigest(),
            cases=cases, summary=summaries[0], peer=fixture.snapshot(), dns=dict(host=dns_host, operator_ipv4=dns_ip))
        import os
        output = os.environ.get('TEST_UNDECLARED_OUTPUTS_DIR')
        if output:
            Path(output, 'qualified.json').write_text(json.dumps(receipt, indent=2)+'\n')
