import { connectFirmware } from '@gizclaw/firmware';

const check = (value, message) => { if (!value) throw new Error(message); };
const rejects = async (operation, code, rpcCode) => {
  try { await operation(); } catch (error) {
    check(error.code === code, 'expected ' + code + ', got ' + error.code);
    if (rpcCode !== undefined) check(error.rpcCode === rpcCode, 'wire error code');
    return;
  }
  throw new Error('operation unexpectedly succeeded');
};
const wait = async predicate => {
  const deadline = performance.now() + 15000;
  while (!(await predicate())) {
    check(performance.now() < deadline, 'provider progress deadline');
    await new Promise(resolve => setTimeout(resolve, 5));
  }
};
const deferred = () => { let resolve; const promise = new Promise(done => {resolve = done;}); return {promise, resolve}; };
const frames = Object.fromEntries(['primary', 'secondary', 'viewer'].map(id => [id, document.getElementById(id)]));
const ready = new Map([frames.primary, frames.secondary].map(frame => [frame, deferred()]));
const finals = new Map([frames.primary, frames.secondary].map(frame => [frame, deferred()]));
const clients = new Map(), observers = new Map();
const viewerFinal = deferred();

window.addEventListener('message', event => {
  const frame = Object.values(frames).find(frame => frame.contentWindow === event.source);
  if (event.data?.type?.startsWith('gizclaw-fixture')) console.log('SDK fixture_message', event.data.type, event.origin, frame?.id);
  if (!frame || event.origin !== new URL(frame.src).origin) return;
  if (frame === frames.viewer && event.data?.type === 'gizclaw-fixture-final') {
    viewerFinal.resolve(event.data); return;
  }
  if (event.data?.type !== 'gizclaw-fixture-ready' || observers.has(frame) || !ready.has(frame)) return;
  const channel = new MessageChannel(), port = channel.port1;
  let serial = 0;
  const pending = new Map();
  const observer = {
    request(type, fields) {
      const id = ++serial;
      return new Promise(resolve => {
        pending.set(id, resolve);
        port.postMessage({type, id, ...fields});
      });
    },
    read: field => observer.request('read', {field}),
    gate: value => observer.request('gate', {value}),
    corrupt: value => observer.request('corrupt', {value}),
    finish: rc => port.postMessage({type: 'finish', rc}),
  };
  observers.set(frame, observer);
  port.onmessage = async event => {
    const message = event.data;
    if (message.type === 'ready') { console.log('SDK observer_ready', frame.id); observer.info = message; ready.get(frame).resolve(observer); }
    else if (message.type === 'result') {
      const done = pending.get(message.id); pending.delete(message.id); done?.(message.value);
    } else if (message.type === 'closed') {
      let rc = 0;
      try { await rejects(() => clients.get(frame).tools.list({timeout_ms: 1000}), -10); }
      catch (error) { console.error('SDK failure close', error); rc = 1; }
      port.postMessage({type: 'closed', rc});
    } else if (message.type === 'final') {
      finals.get(frame).resolve(message); port.close();
    }
  };
  port.start();
  frame.contentWindow.postMessage({type: 'gizclaw-fixture-connect'}, event.origin, [channel.port2]);
});

async function versionAndSource(protocol) {
  await rejects(() => connectFirmware(frames.primary, {origin: '*'}), -1);
  const faux = document.createElement('iframe');
  faux.src = location.href;
  faux.srcdoc = '<script>addEventListener("message",event=>{const m=event.data;if(m?.type==="probe")event.source.postMessage({channel:m.channel,key:m.key,type:"available",version:99},event.origin)});</script>';
  document.body.append(faux);
  await new Promise(resolve => faux.addEventListener('load', resolve, {once: true}));
  await rejects(() => connectFirmware(faux, {timeout_ms: 1000}), -3);
  faux.remove();

  const attacker = document.createElement('iframe');
  attacker.src = location.href;
  attacker.srcdoc = '<script>let answered=false;addEventListener("message",event=>{const m=event.data;if(m?.type==="available")answered=true;if(m?.type==="attack"){parent.frames[0].postMessage(m.probe,m.origin);setTimeout(()=>parent.postMessage({type:"attack-result",answered},"*"),80)}});</script>';
  document.body.append(attacker);
  await new Promise(resolve => attacker.addEventListener('load', resolve, {once: true}));
  const outcome = new Promise(resolve => {
    const listener = event => {
      if (event.source === attacker.contentWindow && event.data?.type === 'attack-result') {
        window.removeEventListener('message', listener); resolve(event.data.answered);
      }
    };
    window.addEventListener('message', listener);
  });
  attacker.contentWindow.postMessage({type: 'attack', origin: new URL(frames.primary.src).origin,
    probe: {...protocol, type: 'probe', key: 'sibling-must-not-connect'}}, location.origin);
  check(!(await outcome), 'firmware rejected a sibling window');
  attacker.remove();

  const loading = document.createElement('iframe');
  loading.src = location.href;
  loading.srcdoc = '<body>No endpoint</body>';
  document.body.append(loading);
  await new Promise(resolve => loading.addEventListener('load', resolve, {once:true}));
  await rejects(() => connectFirmware(loading, {timeout_ms:30}), -6);
  loading.srcdoc = '<script>addEventListener("message",event=>{const m=event.data;if(m?.type==="probe")event.source.postMessage({channel:m.channel,key:m.key,type:"available",version:m.version},event.origin);if(m?.type==="connect")event.ports[0].postMessage({type:"ready",key:m.key,version:m.version,request_capacity:4,request_limit_bytes:65536,timeout_ms:30000})});</script>';
  await new Promise(resolve => loading.addEventListener('load', resolve, {once:true}));
  const navigation = await connectFirmware(loading, {timeout_ms:1000});
  const pending = navigation.call(1, new Uint8Array()).then(() => 0, error => error.code);
  loading.srcdoc = '<body>Navigated</body>';
  check(await pending === -10, 'iframe navigation rejects pending SDK call');
  await rejects(() => navigation.tools.list(), -10);

  loading.srcdoc = '<script>addEventListener("message",event=>{const m=event.data;if(m?.type==="probe")event.source.postMessage({channel:m.channel,key:m.key,type:"available",version:m.version},event.origin);if(m?.type==="connect"){const p=event.ports[0];p.onmessage=e=>{if(e.data.type==="request")p.postMessage({type:"response",id:e.data.id,code:e.data.method===2?1099511627776:0,hasError:false,rpcCode:0,rpcMessage:"",payload:new Uint8Array([1,2,3]),nativeTools:null})};p.start();p.postMessage({type:"ready",key:m.key,version:m.version,request_capacity:4,request_limit_bytes:1,timeout_ms:30000})}});</script>';
  await new Promise(resolve => loading.addEventListener('load', resolve, {once:true}));
  const bounded = await connectFirmware(loading, {timeout_ms:1000});
  check((await bounded.call(1, new Uint8Array())).length === 3, 'request byte limit does not truncate valid replies');
  await rejects(() => bounded.call(1, new Uint8Array(2)), -13);
  await rejects(() => bounded.call(2, new Uint8Array()), -15);
  bounded.close();
  loading.remove();
  console.log('SDK version_source_checks=PASS');
}

async function run() {
  const [observer, other] = await Promise.all([ready.get(frames.primary).promise, ready.get(frames.secondary).promise]);
  check(observer.info.isolated && other.info.isolated && !observer.info.hasSDK && !other.info.hasSDK,
    'firmware has no controller SDK');
  check(observer.info.origin !== other.info.origin, 'actual cross-origin firmware frame');
  console.log('SDK connecting_controllers');
  let api = await connectFirmware(frames.primary);
  console.log('SDK primary_connected');
  const second = await connectFirmware(frames.secondary);
  console.log('SDK secondary_connected');
  clients.set(frames.primary, api); clients.set(frames.secondary, second);
  check(api.version === 1, 'shared transport ABI version');
  const tools = await api.tools.list();
  for (const name of ['info.get','device.find','social.ping','lua.app.list','lua.app.install','lua.app.uninstall'])
    check(tools.some(tool => tool.name === name), 'existing tool ' + name);
  check((await api.tools.invoke('info.get', {})).model === 'local-fixture', 'existing info handler');

  const request = {id:'led.fixture',hwd:'led',payload:{brightness_percent:67,enabled:true}};
  const copied = api.mhs.write(request);
  request.payload.brightness_percent = 99;
  await copied;
  check(await observer.read(6) === 67 && await observer.read(7) === 1, 'copied request reaches real MHS');
  let value = (await api.mhs.write({id:'led.fixture',hwd:'led',payload:{brightness_percent:0,enabled:false}})).payload.applied;
  check(value.brightness_percent === 0 && value.enabled === false, 'applied zero/false');
  value = (await api.mhs.read({id:'led.fixture',hwd:'led'})).payload;
  check(Object.hasOwn(value,'brightness_percent') && value.brightness_percent === 0 &&
        Object.hasOwn(value,'enabled') && value.enabled === false, 'presence');
  const writes = await observer.read(2);
  await rejects(() => api.mhs.write({id:'led.fixture',hwd:'led',payload:{brightness_percent:101}}), -1, 3);
  check(await observer.read(2) === writes, 'invalid input never invokes provider');
  await rejects(() => api.mhs.read({id:'missing.fixture',hwd:'led'}), -8, 5);
  await rejects(() => api.tools.invoke('social.ping', {from_peer_public_key:'fixture'}), -1234, 13);
  await rejects(() => api.call(9999, new Uint8Array()), -3, 12);
  const raw = new Uint8Array([0x0a,0xff]);
  const invalid = api.call(133, raw);
  check(raw.byteLength === 2 && raw[0] === 0x0a, 'caller bytes are not transferred/detached');
  raw[0] = 0;
  await rejects(() => invalid, -1000, 3);

  check((await api.tools.invoke('lua.app.list', {})).apps.length === 0, 'existing empty catalog');
  const installed = await api.tools.invoke('lua.app.install', {url:'https://fixture.invalid/demo'});
  check(installed.app.app_id === 'demo' && installed.app.version === '0.2.0', 'existing installer');
  check((await api.tools.invoke('lua.app.list', {})).apps[0].version === '0.2.0', 'existing list');
  await api.tools.invoke('lua.app.uninstall', {app_id:'demo'});
  check((await api.tools.invoke('lua.app.list', {})).apps.length === 0, 'existing uninstall');
  console.log('SDK existing_lua_tools=PASS');

  await observer.gate(1);
  let entered = await observer.read(3), completed = await observer.read(4);
  let slow = api.tools.invoke('device.find', {});
  await wait(async () => await observer.read(3) === entered + 1);
  const reads = await observer.read(1), controllers = [], queued = [];
  for (let i = 0; i < 3; ++i) {
    const controller = new AbortController(); controllers.push(controller);
    queued.push(api.mhs.read({id:'led.fixture',hwd:'led'}, {signal:controller.signal}).then(
      () => 0, error => error.code));
  }
  await rejects(() => api.mhs.read({id:'led.fixture',hwd:'led'}), -18);
  controllers.forEach(controller => controller.abort());
  check((await Promise.all(queued)).every(code => code === -10), 'queued cancellation');
  await observer.gate(0); await slow;
  await wait(async () => await observer.read(4) === completed + 1);
  check(await observer.read(1) === reads && await observer.read(5) === 0, 'one acknowledged completion; no canceled provider call');
  console.log('SDK iframe_tools_mhs=PASS');

  await second.mhs.write({id:'led.fixture',hwd:'led',payload:{brightness_percent:29,enabled:true}});
  check((await second.mhs.read({id:'led.fixture',hwd:'led'})).payload.brightness_percent === 29, 'second real handler');
  check((await api.mhs.read({id:'led.fixture',hwd:'led'})).payload.brightness_percent === 0 &&
        await observer.read(6) === 0 && await other.read(6) === 29, 'independent frame state');
  console.log('SDK cross_origin_isolation=PASS');

  await observer.gate(1);
  entered = await observer.read(3); completed = await observer.read(4);
  slow = api.tools.invoke('device.find', {});
  await wait(async () => await observer.read(3) === entered + 1);
  const before = await observer.read(1);
  await rejects(() => api.mhs.read({id:'led.fixture',hwd:'led'}, {timeout_ms:35}), -6);
  await observer.gate(0); await slow;
  await wait(async () => await observer.read(4) === completed + 1);
  check(await observer.read(1) === before, 'timed-out queued call never runs');

  await observer.gate(1);
  entered = await observer.read(3); completed = await observer.read(4);
  const closed = api.tools.invoke('device.find', {}).then(() => 0, error => error.code);
  await wait(async () => await observer.read(3) === entered + 1);
  api.close(); api.close();
  check(await closed === -10, 'close rejects pending request');
  await observer.gate(0);
  await wait(async () => await observer.read(4) === completed + 1);
  check(await observer.read(5) !== 0, 'disconnect fails delivery continuation');
  api = await connectFirmware(frames.primary);
  clients.set(frames.primary, api);
  check((await api.tools.invoke('info.get', {})).model === 'local-fixture', 'reconnect keeps firmware running');

  await observer.gate(1);
  entered = await observer.read(3); completed = await observer.read(4);
  const replaced = api.tools.invoke('device.find', {}).then(() => 0, error => error.code);
  await wait(async () => await observer.read(3) === entered + 1);
  const previous = api;
  api = await connectFirmware(frames.primary);
  clients.set(frames.primary, api);
  check(await replaced === -10, 'new controller invalidates old pending request');
  await rejects(() => previous.tools.list(), -10);
  await observer.gate(0);
  await wait(async () => await observer.read(4) === completed + 1);
  check(await observer.read(5) !== 0, 'replaced controller fails old delivery');
  check((await api.tools.invoke('info.get', {})).model === 'local-fixture', 'replacement uses same existing handler');
  console.log('SDK reconnect_cancel=PASS');

  completed = await observer.read(4);
  await observer.corrupt(1);
  await rejects(() => api.tools.invoke('device.find', {}), -15);
  await wait(async () => await observer.read(4) === completed + 1);
  check(await observer.read(5) === -15, 'failed SDK decode fails native delivery acknowledgement');
  console.log('SDK decode_ack=PASS');

  await versionAndSource(await (await fetch('./protocol.json')).json());
  const viewer = await viewerFinal.promise;
  check(viewer.rc === 0 && viewer.destroy === 0, 'display-only iframe ran without SDK');
  console.log('SDK view_only_without_sdk=PASS');
  observer.finish(0); other.finish(0);
  const final = await Promise.all([...finals.values()].map(done => done.promise));
  check(final.every(value => value.rc === 0 && value.destroy === 0), 'native close retry and complete teardown');
  console.log('SDK all_frames_closed=PASS');
}

document.getElementById('start').addEventListener('click', () => {
  const local = new URL('./firmware/index.html', location.href);
  local.searchParams.set('fixture-auto', '1');
  frames.primary.src = local.href;
  const remote = new URL(local);
  remote.hostname = remote.hostname === '127.0.0.1' ? 'localhost' : '127.0.0.1';
  frames.secondary.src = remote.href;
  const viewer = new URL(local); viewer.searchParams.set('standalone', '1');
  frames.viewer.src = viewer.href;
  run().catch(async error => {
    console.error('SDK failure', error.stack || String(error), error.cause || '');
    for (const observer of observers.values()) { await observer.gate(0); observer.finish(1); }
  });
}, {once:true});
