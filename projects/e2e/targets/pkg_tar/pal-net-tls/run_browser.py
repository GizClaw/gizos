"""Execute actual Worker/AppHost raw Net capability calls; never qualify TLS."""
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer
sys.path.insert(0,str(Path('tools/bazel').resolve()))
from web_archive_browser_test import Cdp, find_browser
from web_archive_server import prepared_archive, make_handler, read_header_policy

archive=Path(sys.argv[1])
with prepared_archive(archive.resolve()) as root, tempfile.TemporaryDirectory(prefix='pal-net-tls-browser-') as temporary:
    server=ThreadingHTTPServer(('127.0.0.1',0),make_handler(root,read_header_policy(root)))
    thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    incoming,outgoing=os.pipe(),os.pipe()
    events=queue.Queue()
    def pipes():
        read,write=os.dup(incoming[0]),os.dup(outgoing[1])
        os.dup2(read,3);os.dup2(write,4)
    browser=find_browser()
    process=subprocess.Popen([str(browser),'--headless','--no-sandbox','--remote-debugging-pipe',
        '--no-first-run','--no-default-browser-check','--disable-background-networking',
        '--user-data-dir='+temporary+'/profile','about:blank'],preexec_fn=pipes,pass_fds=(3,4),stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    os.close(incoming[0]);os.close(outgoing[1])
    cdp=Cdp(incoming[1],outgoing[0],events)
    lines=[];caps=[];boundary=None
    try:
        target=cdp.send('Target.createTarget',{'url':'about:blank'})['targetId']
        session=cdp.send('Target.attachToTarget',{'targetId':target,'flatten':True})['sessionId']
        cdp.send('Runtime.enable',session=session)
        cdp.send('Page.enable',session=session)
        cdp.send('Page.navigate',{'url':f'http://127.0.0.1:{server.server_port}/'},session=session)
        deadline=time.monotonic()+120
        while time.monotonic()<deadline:
            try:event=events.get(timeout=.5)
            except queue.Empty:continue
            if event.get('method')=='Runtime.exceptionThrown':raise RuntimeError(event)
            if event.get('method')!='Runtime.consoleAPICalled':continue
            line=' '.join(str(arg.get('value',arg.get('description',''))) for arg in event['params']['args'])
            print(line,flush=True);lines.append(line)
            if 'Aborted(' in line:raise RuntimeError(line)
            if line.startswith('H2_PAL_NET_TLS_CAPABILITY '):caps.append(json.loads(line.split(' ',1)[1]))
            if line.startswith('H2_PAL_NET_TLS_WEB_BOUNDARY '):boundary=json.loads(line.split(' ',1)[1])
            if line.startswith('H2_NET_TLS_EXIT '):
                assert line=='H2_NET_TLS_EXIT 0'
                break
        else:raise TimeoutError('browser capability boundary did not complete')
        assert boundary and boundary['worker']==1 and boundary['main_runtime_thread']==0
        assert boundary['operations']==21 and boundary['unexpected_results']==0
        assert not boundary['core_qualified'] and not boundary['full_net_qualified']
        assert len(caps)==21 and len({cap['slot'] for cap in caps})==21
        assert all(cap['status'] in ('UNSUPPORTED','UNSUPPORTED_NOOP') for cap in caps)
        assert {cap['slot'] for cap in caps if cap['status']=='UNSUPPORTED_NOOP'} == {'close','resolve_close'}
        assert any('result=PASS rc=0 fs=0 destroy=0' in line for line in lines),lines
        isolated=cdp.send('Runtime.evaluate',{'expression':'crossOriginIsolated === true','returnByValue':True},session=session)['result']['value']
        assert isolated is True
        boundary.update(capabilities=caps,artifact_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
            browser_sha256=hashlib.sha256(Path(browser).read_bytes()).hexdigest(),browser=str(browser),
            cross_origin_isolated=True,teardown=0)
        output=os.environ.get('TEST_UNDECLARED_OUTPUTS_DIR')
        if output:Path(output,'boundary.json').write_text(json.dumps(boundary,indent=2)+'\n')
    finally:
        if process.poll() is None:
            process.terminate()
        try:process.wait(timeout=10)
        except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
        server.shutdown();server.server_close();thread.join(timeout=2)
