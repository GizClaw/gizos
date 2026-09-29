import copy
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

    def test_peer_missing_certificate_cannot_pass(self):
        session='a'*32
        run_id=session[:16]
        rows=[dict(session=session,run_id=run_id,case=case,accepted=True,client_hello=True,certificate_presented=True,
            finished=True,handshake_succeeded=False,payload_received=0,payload_sent=0) for case in ('tls-default-untrusted','tls-wrong-ca','tls-wrong-name','tls-expired')]
        rows.append(dict(session=session,run_id=run_id,case='tls-sni-alpn',sni='pal-net-tls.test',alpn='h2-pal-e2e'))
        valid=dict(session=session,records=rows)
        validation.check_peer(valid,session,run_id)
        for field,value in [('accepted',False),('client_hello',False),('certificate_presented',False),('finished',False),('handshake_succeeded',True),('payload_received',1),('session','b'*32)]:
            bad=copy.deepcopy(valid);bad['records'][0][field]=value
            with self.assertRaises(AssertionError):validation.check_peer(bad,session,run_id)
        bad=copy.deepcopy(valid);bad['records'].append(bad['records'][0])
        with self.assertRaises(AssertionError):validation.check_peer(bad,session,run_id)


if __name__=='__main__':unittest.main()
