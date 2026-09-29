import copy
import json
import importlib.util
from pathlib import Path
import re
import unittest

APP=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('qualification',APP/'check_qualification.py')
validation=importlib.util.module_from_spec(spec)
spec.loader.exec_module(validation)
REGISTRY=re.findall(r'H2_NET_TLS_CASE\(\w+, "([^"]+)", ([01])\)',(APP/'app/include/h2_pal_net_tls_cases.inc').read_text())


def receipt():
    rows=[dict(id=name,mandatory=bool(int(required)),status='PASS' if int(required) else 'UNSUPPORTED',
        detail=0,provider_result=validation.TLS_VERIFY if name in ('tls-default-untrusted','tls-wrong-ca','tls-wrong-name','tls-expired') else 0,
        observed_ipv4=[150,5,151,236] if name=='dns-hostname' else [0,0,0,0],bytes_sent=4193 if name.startswith('tls-') else 0,bytes_received=4097 if name.startswith('tls-') else 0) for name,required in REGISTRY]
    return dict(dns=dict(host='dns.test',operator_ipv4='150.5.151.236'),cases=rows,summary=dict(core_qualified=True,full_net_qualified=False,mandatory_passed=37,
        failed=0,blocked=0,retained_sockets=0,retained_resolvers=0,retained_allocations=0,rc=0,teardown=0))


class Rejection(unittest.TestCase):
    def test_typed_evidence_required(self):
        valid=receipt()
        validation.check_cases(valid,REGISTRY)
        for field,value in [('provider_result',-1),('status','BLOCKED'),('detail',-1)]:
            bad=copy.deepcopy(valid)
            bad['cases'][24][field]=value
            with self.assertRaises(AssertionError):validation.check_cases(bad,REGISTRY)
        for mutate in [lambda rows:rows.pop(),lambda rows:rows.reverse(),lambda rows:rows.append(rows[0]),lambda rows:rows[0].update(id='missing')]:
            bad=copy.deepcopy(valid);mutate(bad['cases'])
            with self.assertRaises(AssertionError):validation.check_cases(bad,REGISTRY)
        bad=copy.deepcopy(valid);bad['summary']['retained_sockets']=1
        with self.assertRaises(AssertionError):validation.check_cases(bad,REGISTRY)
        bad=copy.deepcopy(valid);bad['cases'][21]['bytes_received']=4096
        with self.assertRaises(AssertionError):validation.check_cases(bad,REGISTRY)

    def test_browser_unsupported_boundary_is_not_core_qualification(self):
        browser=json.loads((APP/'evidence/wasm/boundary.json').read_text())
        self.assertFalse(browser['core_qualified'])
        self.assertEqual(len(browser['capabilities']),21)
        with self.assertRaises((AssertionError, KeyError)):
            validation.check_cases(browser,REGISTRY)

    def test_peer_missing_certificate_or_payload_cannot_pass(self):
        receipt=json.loads((APP/'evidence/macos/qualified.json').read_text())
        peer=receipt['peer']
        session=peer['session']
        run_id=session[:16]
        validation.check_peer(peer,session,run_id)
        for field,value in [('accepted',False),('client_hello',False),
                            ('certificate_presented',False),('finished',False),
                            ('payload_received',1),('payload_sent',1),
                            ('session','b'*32)]:
            bad=copy.deepcopy(peer)
            target=next(row for row in bad['records'] if row['case']=='tls-wrong-ca')
            target[field]=value
            with self.assertRaises(AssertionError):
                validation.check_peer(bad,session,run_id)
        for case, field, value in [('tls-required','payload_received',4096),
                                   ('tcp-echo','payload_valid',False),
                                   ('udp-echo','payload_sent',512)]:
            bad=copy.deepcopy(peer)
            target=next(row for row in bad['records'] if row['case']==case)
            target[field]=value
            with self.assertRaises(AssertionError):
                validation.check_peer(bad,session,run_id)
        for case in ('tls-wrong-ca','tls-session-churn'):
            bad=copy.deepcopy(peer)
            target=next(row for row in bad['records'] if row['case']==case)
            bad['records'].append(copy.deepcopy(target))
            with self.assertRaises(AssertionError):
                validation.check_peer(bad,session,run_id)


if __name__=='__main__':unittest.main()
