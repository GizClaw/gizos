import importlib.util
import json
from pathlib import Path
import re
import unittest

ROOT=Path(__file__).resolve().parents[5]
DEVICE=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('device_evidence',DEVICE/'verify_device.py')
validation=importlib.util.module_from_spec(spec);spec.loader.exec_module(validation)
REGISTRY=re.findall(r'H2_NET_TLS_CASE\(\w+, "([^"]+)", ([01])\)',(ROOT/'projects/e2e/apps/pal-net-tls/app/include/h2_pal_net_tls_cases.inc').read_text())


def log():
    lines=['H2_PAL_NET_TLS_BOOT board=bk7258 version=test-version',
        'H2_PAL_NET_TLS_EXECUTION session='+'a'*32+' boot_id='+'b'*16]
    lines.extend('H2_PAL_NET_TLS_CASE '+json.dumps(dict(id=name,mandatory=bool(int(required)),status='PASS' if int(required) else 'NOT_ASSESSED',detail=0)) for name,required in REGISTRY)
    lines.append('H2_PAL_NET_TLS_RUN session='+'a'*32+' boot_id='+'b'*16+' version=test-version version_rc=0 dns_host=dns.test dns_ipv4=150.5.151.236')
    lines.append('H2_PAL_NET_TLS_SUMMARY '+json.dumps(dict(mandatory_passed=sum(int(required) for _,required in REGISTRY),full_net_qualified=False,failed=0,blocked=0,retained_sockets=0,retained_resolvers=0,retained_allocations=0)))
    lines.append('H2_PAL_NET_TLS_READY board=bk7258 rc=0 confirm=0')
    return '\n'.join(lines)


class FreshBoot(unittest.TestCase):
    def test_complete(self):
        self.assertEqual(len(validation.parse(log(),'test-version','bk7258')['cases']),len(REGISTRY))
    def test_missing_execution_cannot_be_replay(self):
        text=log()
        for damaged in [text.replace('H2_PAL_NET_TLS_EXECUTION','REPLAY'),text+'\n'+text,
            text.replace('"id": "dns-sync"','"id": "duplicate"'),text.replace('confirm=0','confirm=-1'),
            text.replace('boot_id='+'b'*16,'boot_id='+'c'*16,1),
            text.replace('"status": "PASS"','"status": "BLOCKED"',1),
            '\n'.join(line for line in text.splitlines() if '"id": "tcp-echo"' not in line)]:
            with self.assertRaises(AssertionError):validation.parse(damaged,'test-version','bk7258')


if __name__=='__main__':unittest.main()
