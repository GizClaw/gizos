"""Serve one explicitly bound LAN fixture and freeze complete per-boot witnesses."""
import argparse
from datetime import datetime, timezone
import hashlib
import ipaddress
import json
from pathlib import Path
import tempfile
import time
from mqtt_fixture import Fixture

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--bind',required=True)
    parser.add_argument('--advertised',required=True)
    parser.add_argument('--bazelrc',required=True,type=Path)
    parser.add_argument('--receipt',required=True,type=Path)
    parser.add_argument('--evidence',required=True,type=Path)
    args=parser.parse_args()
    for value in (args.bind,args.advertised):
        ip=ipaddress.ip_address(value)
        if ip.version!=4 or ip.is_unspecified:raise ValueError('explicit IPv4 interface address required')
    with tempfile.TemporaryDirectory(prefix='h2-mqtt-lan-') as directory,Fixture(directory,bind=args.bind,advertised=args.advertised) as fixture:
        epoch=int(time.time()*1000)
        args.evidence.mkdir(parents=True,exist_ok=True)
        (args.evidence/'ca.pem').write_bytes(fixture.ca.read_bytes())
        (args.evidence/'wrong-ca.pem').write_bytes(fixture.wrong_ca.read_bytes())
        values=dict(HOST=args.advertised,TCP_PORT=str(fixture.tcp.port),TLS_PORT=str(fixture.tls.port),
            SESSION_PREFIX=fixture.session,CA_PEM_HEX=fixture.ca.read_bytes().hex(),
            WRONG_CA_PEM_HEX=fixture.wrong_ca.read_bytes().hex(),FIXTURE_EPOCH_MS=str(epoch))
        args.bazelrc.parent.mkdir(parents=True,exist_ok=True)
        args.bazelrc.write_text(''.join('build --define=H2_PAL_MQTT_'+key+'='+value+'\n' for key,value in values.items()))
        (args.evidence/'fixture.bazelrc').write_bytes(args.bazelrc.read_bytes())
        (args.evidence/'defines.json').write_text(json.dumps(values,indent=2)+'\n')
        inputs=dict(bind=args.bind,advertised=args.advertised,tcp_port=fixture.tcp.port,tls_port=fixture.tls.port,
            session_prefix=fixture.session,epoch_ms=epoch,ca_sha256=hashlib.sha256(fixture.ca.read_bytes()).hexdigest(),
            wrong_ca_sha256=hashlib.sha256(fixture.wrong_ca.read_bytes()).hexdigest(),
            bazelrc_sha256=hashlib.sha256(args.bazelrc.read_bytes()).hexdigest())
        (args.evidence/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
        completed={}
        args.receipt.parent.mkdir(parents=True,exist_ok=True)
        print(json.dumps(dict(status='ready',**inputs,bazelrc=str(args.bazelrc),receipt=str(args.receipt))),flush=True)
        try:
            while True:
                with fixture.tcp.lock:
                    sessions={client[:-len('-connect-events')] for client in fixture.tcp.arrivals if client.endswith('-connect-events')}
                    arrivals=dict(fixture.tcp.arrivals)
                progress={}
                for session in sessions:
                    if session in completed:continue
                    try:
                        witness=fixture.verify(session=session)
                    except RuntimeError as error:
                        progress[session]=dict(verified=False,reason=str(error))
                    else:
                        witness.update(verified=True,observed_at_utc=datetime.now(timezone.utc).isoformat())
                        completed[session]=witness
                data=dict(inputs=inputs,runs={**progress,**completed},arrivals=arrivals)
                temporary=args.receipt.with_suffix('.tmp');temporary.write_text(json.dumps(data,indent=2)+'\n');temporary.replace(args.receipt)
                time.sleep(1)
        except KeyboardInterrupt:
            pass
if __name__=='__main__':main()
