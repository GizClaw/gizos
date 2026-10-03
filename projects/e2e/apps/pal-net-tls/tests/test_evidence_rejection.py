import copy
import json
import importlib.util
from pathlib import Path
import re
import unittest
import tempfile

APP=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('qualification',APP/'check_qualification.py')
validation=importlib.util.module_from_spec(spec)
spec.loader.exec_module(validation)
REGISTRY=re.findall(r'H2_NET_TLS_CASE\(\w+, "([^"]+)", ([01])\)',(APP/'app/include/h2_pal_net_tls_cases.inc').read_text())


def receipt():
    rows=[dict(id=name,mandatory=bool(int(required)),status='PASS' if int(required) else 'UNSUPPORTED',
        detail=0 if int(required) else validation.UNSUPPORTED,provider_result=validation.TLS_VERIFY if name in ('tls-default-untrusted','tls-wrong-ca','tls-wrong-name','tls-expired') else 0,
        observed_ipv4=[150,5,151,236] if name=='dns-hostname' else [0,0,0,0],bytes_sent=4193 if name.startswith('tls-') else 0,bytes_received=4097 if name.startswith('tls-') else 0) for name,required in REGISTRY]
    return dict(dns=dict(host='dns.test',operator_ipv4='150.5.151.236',observation=dict(hostname='dns.test',ipv4='150.5.151.236',resolver='independent test oracle',method='fixture observation',observed_at_utc='2026-10-01T00:00:00+00:00')),cases=rows,summary=dict(core_qualified=True,full_net_qualified=False,mandatory_passed=37,passed=37,unsupported=2,not_assessed=0,
        failed=0,blocked=0,retained_sockets=0,retained_resolvers=0,retained_allocations=0,rc=0,teardown=0))


class Rejection(unittest.TestCase):
    def test_followup_cannot_replace_other_provider_or_claim_fresh_hardware(self):
        followup=json.loads((APP/'gizclaw_public_https_provenance_main_tls.json').read_text())
        historical=json.loads((APP/'qualification.json').read_text())['source_sha256']
        validation.check_sources(validation.ROOT,historical,followup)
        for mutate in [
            lambda value:value.update(new_physical_run_claimed=True),
            lambda value:value['current_source_sha256'].update({'libs/pal/providers/ios/pal_core/src/h2_ios_net.c':'0'*64}),
            lambda value:value['previous_source_sha256'].update({'tools/bazel/mobile_e2e.py':'0'*64}),
            lambda value:value['current_source_sha256'].update({'tools/bazel/mobile_e2e.py':'0'*64}),
        ]:
            bad=copy.deepcopy(followup);mutate(bad)
            with self.assertRaises(AssertionError):validation.check_sources(validation.ROOT,historical,bad)

    def test_every_native_provider_source_is_mandatory(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            import hashlib
            sources={}
            for relative in validation.REQUIRED_PROVIDER_SOURCES:
                path=root/relative;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'provider')
                sources[relative]=hashlib.sha256(path.read_bytes()).hexdigest()
            validation.check_sources(root,sources)
            for relative in sources:
                bad=dict(sources);bad.pop(relative)
                with self.assertRaises(AssertionError):validation.check_sources(root,bad)
            next(iter(root.rglob('h2_ios_net.c'))).write_bytes(b'changed provider')
            with self.assertRaises(AssertionError):validation.check_sources(root,sources)

    def test_structured_board_observations_reject_state_or_dump_corruption(self):
        board=json.loads((APP/'evidence/bk7258/qualified.json').read_text())
        validation.check_board_observation(board)
        for section,key,value in [('observed_status','stage_valid','1'),('observed_status','partition_1_image_checksum','0'*64),('observed_status','device_uid','wrong'),('observed_coredump','stored_bytes','64'),('observed_boot','boot_id','0'*16)]:
            bad=copy.deepcopy(board);bad['boots'][0][section][key]=value
            with self.assertRaises(AssertionError):validation.check_board_observation(bad)
        bad=copy.deepcopy(board);bad['observed_coredump_bytes']['install']='00'*32
        with self.assertRaises(AssertionError):validation.check_board_observation(bad)

    def test_esp_family_retains_exact_execution_board_and_dump_bytes(self):
        board = json.loads((APP/'evidence/bk7258/qualified.json').read_text())
        board['platform'] = 'esp32s3'
        board['execution_board'] = 'zero_esp_v3_0'
        states = [board['observed_baseline']['status']] + [boot['observed_status'] for boot in board['boots']]
        for status in states:
            status.update(board='zero_esp_v3_0', target='esp32s3')
        for boot in board['boots']:
            boot['observed_boot']['board'] = 'zero_esp_v3_0'
        validation.check_board_observation(board)
        bad = copy.deepcopy(board)
        bad['boots'][0]['observed_boot']['board'] = 'devkit'
        with self.assertRaises(AssertionError):
            validation.check_board_observation(bad)
        bad = copy.deepcopy(board)
        bad['observed_coredump_bytes']['install'] = '00' * 32
        with self.assertRaises(AssertionError):
            validation.check_board_observation(bad)

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
        slots=json.loads((APP/'app/api_coverage.json').read_text())['slots']
        validation.check_browser(browser,slots)
        self.assertFalse(browser['core_qualified'])
        self.assertEqual(len(browser['capabilities']),21)
        with self.assertRaises((AssertionError, KeyError)):
            validation.check_cases(browser,REGISTRY)
        for field,value in [('core_qualified',True),('worker',0),
                            ('main_runtime_thread',1),('teardown',1),
                            ('cross_origin_isolated',False),('unexpected_results',1)]:
            bad=copy.deepcopy(browser);bad[field]=value
            with self.assertRaises(AssertionError):validation.check_browser(bad,slots)
        for mutate in [lambda rows:rows.pop(),lambda rows:rows.append(rows[0]),
                       lambda rows:rows[0].update(slot='unknown'),
                       lambda rows:rows[0].update(result=-1),
                       lambda rows:rows[0].update(status='PASS'),
                       lambda rows:next(row for row in rows if row['slot']=='close').update(owned_handle=True)]:
            bad=copy.deepcopy(browser);mutate(bad['capabilities'])
            with self.assertRaises(AssertionError):validation.check_browser(bad,slots)

    def test_supported_optional_capability_cannot_remain_unassessed(self):
        bad=receipt()
        next(row for row in bad['cases'] if row['id']=='icmp-echo')['status']='NOT_ASSESSED'
        with self.assertRaises(AssertionError):validation.check_cases(bad,REGISTRY)

    def test_dns_nonzero_mismatch_does_not_pass(self):
        bad=receipt()
        next(row for row in bad['cases'] if row['id']=='dns-hostname')['observed_ipv4']=[150,5,151,237]
        with self.assertRaises(AssertionError):validation.check_cases(bad,REGISTRY)

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
