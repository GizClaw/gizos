"""Require one freshly executing boot; immutable replay cannot replace it."""
import argparse
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[4]
REGISTRY=ROOT/'projects/e2e/apps/pal-net-tls/app/include/h2_pal_net_tls_cases.inc'


def parse(text, expected_version, board):
    text=re.sub(r'\x1b\[[0-9;]*[A-Za-z]','',text).replace('\r','')
    markers=[]
    for line in text.splitlines():
        match=re.search(r'(H2_PAL_NET_TLS_[A-Z_]+)\s+(.*)',line)
        if match:markers.append((match[1],match[2]))
    boot=[value for key,value in markers if key=='H2_PAL_NET_TLS_BOOT']
    execution=[value for key,value in markers if key=='H2_PAL_NET_TLS_EXECUTION']
    assert len(boot)==1 and len(execution)==1,'missing/duplicate fresh boot or execution'
    assert 'board='+board in boot[0] and 'version='+expected_version in boot[0]
    match=re.fullmatch(r'session=([0-9a-f]{32}) boot_id=([0-9a-f]{16})',execution[0])
    assert match,'invalid boot identity'
    session,boot_id=match.groups()
    begin=next(i for i,(key,_) in enumerate(markers) if key=='H2_PAL_NET_TLS_EXECUTION')
    replay=next((i for i,(key,_) in enumerate(markers[begin+1:],begin+1) if key=='H2_PAL_NET_TLS_RUN'),None)
    assert replay is not None,'final replay/run metadata missing'
    compact=[json.loads(value) for key,value in markers[begin+1:replay] if key=='H2_PAL_NET_TLS_CASE']
    registry=re.findall(r'H2_NET_TLS_CASE\(\w+, "([^"]+)", ([01])\)',REGISTRY.read_text())
    running=[]
    for row, (_, required) in zip(compact, registry):
        running.append(dict(id=row['id'], mandatory=bool(int(required)),
            status=row['status'], detail=row['detail'], provider_result=row['rc'],
            bytes_sent=row['tx'], bytes_received=row['rx'], elapsed_ms=row['ms'],
            observed_ipv4=row.get('ip',[0,0,0,0])))
    assert len(compact)==len(registry)
    assert [row['id'] for row in running]==[name for name,_ in registry],'incomplete/replayed/reordered executing ledger'
    mandatory=sum(int(required) for _,required in registry)
    assert all(row['status']=='PASS' and row['detail']==0 for row in running if row['mandatory'])
    assert all(row['status'] in ('PASS','UNSUPPORTED','NOT_ASSESSED') for row in running if not row['mandatory'])
    run=markers[replay][1]
    assert 'session='+session in run and 'boot_id='+boot_id in run and 'version='+expected_version in run and 'version_rc=0' in run
    dns_match=re.search(r'dns_host=(\S+) dns_ipv4=(\d+\.\d+\.\d+\.\d+)',run)
    assert dns_match,'operator DNS inputs missing'
    summaries=[json.loads(value) for key,value in markers[replay:] if key=='H2_PAL_NET_TLS_SUMMARY']
    ready=[value for key,value in markers[replay:] if key=='H2_PAL_NET_TLS_READY']
    assert summaries and ready,'no completed summary/confirmation'
    raw=summaries[0]
    summary=dict(profile='net-tls-core', full_net_qualified=False, passed=raw['pass'],
        mandatory_passed=raw['mandatory'],failed=raw['fail'],blocked=raw['blocked'],
        unsupported=raw['unsupported'],not_assessed=raw['not_assessed'],
        retained_sockets=raw['rs'],retained_resolvers=raw['rr'],
        retained_allocations=raw['ra'])
    assert summary['mandatory_passed']==mandatory and not summary['full_net_qualified']
    assert not any(summary[key] for key in ('failed','blocked','retained_sockets','retained_resolvers','retained_allocations'))
    assert re.fullmatch(r'board='+re.escape(board)+r' rc=0 confirm=0',ready[0])
    assert not any(key in ('H2_PAL_NET_TLS_SETUP_FAIL','H2_PAL_NET_TLS_WATCHDOG') for key,_ in markers)
    summary.update(core_qualified=True,operations=21,rc=0,teardown=0)
    return dict(dns=dict(host=dns_match[1], operator_ipv4=dns_match[2]),session=session,boot_id=boot_id,version=expected_version,cases=running,summary=summary,
        teardown_scope='App-owned sockets, resolvers and scratch; Board Runtime remains available for H2Loader')


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--log',type=Path,required=True)
    parser.add_argument('--version',required=True)
    parser.add_argument('--board',choices=['devkit','bk7258','tiga_esp_v4_2','zero_esp_v3_0'],required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    result=parse(args.log.read_text(errors='replace'),args.version,args.board)
    args.output.write_text(json.dumps(result,indent=2)+'\n')
