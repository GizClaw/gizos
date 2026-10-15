import { createFirmwareClient } from '@gizclaw/firmware';

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
async function contractChecks(runtime, observer, tools) {
  await rejects(() => createFirmwareClient({h2GizClaw: null}), -10);
  const abi = runtime.h2GizClaw;
  await rejects(() => createFirmwareClient({h2GizClaw: {...abi, version:99}}), -3);
  await rejects(() => createFirmwareClient({h2GizClaw: {...abi, request_capacity:0}}), -15);
  check(abi.request(136, new Uint8Array(), null) === -1, 'raw admission validates callback');
  check(abi.request(136, new Uint8Array(abi.request_limit_bytes + 1), () => 0) === -13, 'raw request bound');
  let calls = 0, returned = false, retained;
  await new Promise((resolve, reject) => {
    const id = abi.request(136, new Uint8Array(), response => {
      try {
        check(returned && ++calls === 1 && response.code === 0 && response.nativeTools.length > 0,
          'raw API runs without object/Promise SDK');
        retained = response.payload;
        resolve(); return 0;
      } catch (error) { reject(error); return -15; }
    });
    check(id > 0, 'raw request returns a call ID'); returned = true;
  });
  check(retained.length > 0, 'raw reply remains owned after callback');
  check(calls === 1, 'exactly one raw callback');
  const tool = tools.find(tool => tool.name === 'device.find').id;
  const encoded = [];
  for (let value = tool; ; value = Math.floor(value / 128)) {
    encoded.push((value % 128) | (value >= 128 ? 128 : 0));
    if (value < 128) break;
  }
  const invoke = new Uint8Array([8, ...encoded, 18, 0]);
  for (const invalid of [() => { throw new Error('raw consumer failed'); }, () => Promise.resolve(0)]) {
    const before = observer.read(4);
    let received = 0;
    check(abi.request(135, invoke, response => {
      check(response.code === 0, 'raw existing prepared handler');
      ++received;
      return invalid();
    }) > 0, 'raw prepared request admitted');
    await wait(() => observer.read(4) === before + 1);
    check(received === 1 && observer.read(5) === -15, 'throw/async raw callback fails native delivery exactly once');
  }
  console.log('SDK raw_callback_api=PASS');
}

async function run() {
  const runtime = globalThis.Module;
  const frame = document.getElementById('secondary');
  await wait(() => runtime?.h2LocalFixture && frame.contentWindow.Module?.h2LocalFixture);
  const secondRuntime = frame.contentWindow.Module;
  const observer = runtime.h2LocalFixture, other = secondRuntime.h2LocalFixture;
  check(runtime !== secondRuntime && runtime.h2GizClaw !== secondRuntime.h2GizClaw,
    'two actual WASM runtimes');
  let api = createFirmwareClient(runtime);
  const second = createFirmwareClient(secondRuntime);
  check(api.version === 1, 'shared runtime ABI version');
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
  console.log('SDK runtime_tools_mhs=PASS');

  await second.mhs.write({id:'led.fixture',hwd:'led',payload:{brightness_percent:29,enabled:true}});
  check((await second.mhs.read({id:'led.fixture',hwd:'led'})).payload.brightness_percent === 29, 'second real handler');
  check((await api.mhs.read({id:'led.fixture',hwd:'led'})).payload.brightness_percent === 0 &&
        await observer.read(6) === 0 && await other.read(6) === 29, 'independent WASM instance state');
  console.log('SDK runtime_isolation=PASS');

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
  api = createFirmwareClient(runtime);
  check((await api.tools.invoke('info.get', {})).model === 'local-fixture', 'reconnect keeps firmware running');

  // A second client on the same module neither replaces nor owns the first.
  const sibling = createFirmwareClient(runtime);
  await observer.gate(1);
  entered = await observer.read(3); completed = await observer.read(4);
  const canceled = api.tools.invoke('device.find', {}).then(() => 0, error => error.code);
  await wait(async () => await observer.read(3) === entered + 1);
  const surviving = sibling.mhs.read({id:'led.fixture',hwd:'led'});
  api.close();
  check(await canceled === -10, 'only closed client rejects its pending call');
  await observer.gate(0);
  check((await surviving).payload.brightness_percent === 0, 'sibling client keeps working');
  await wait(async () => await observer.read(4) === completed + 1);
  await rejects(() => api.tools.list(), -10);
  api = createFirmwareClient(runtime);
  check((await api.tools.invoke('info.get', {})).model === 'local-fixture', 'same runtime remains active');
  sibling.close();
  console.log('SDK client_close_cancel=PASS');

  completed = await observer.read(4);
  await observer.corrupt(1);
  await rejects(() => api.tools.invoke('device.find', {}), -15);
  await wait(async () => await observer.read(4) === completed + 1);
  check(await observer.read(5) === -15, 'failed SDK decode fails native delivery');
  console.log('SDK decode_ack=PASS');
  await contractChecks(runtime, observer, tools);

  // Native close cancels both raw and SDK requests before a failed join retry.
  await observer.gate(1);
  entered = await observer.read(3);
  const detached = api.tools.invoke('device.find', {}).then(() => 0, error => error.code);
  await wait(async () => await observer.read(3) === entered + 1);
  const oldABI = runtime.h2GizClaw;
  observer.onClosed = async () => {
    check(await detached === -10 && oldABI.closed, 'native detach invalidates outstanding calls and old ABI');
    check(oldABI.request(136, new Uint8Array(), () => 0) === -10, 'old raw reference is closed');
    await rejects(() => api.tools.list(), -10);
    return 0;
  };
  other.onClosed = async () => { await rejects(() => second.tools.list(), -10); return 0; };
  observer.finish(0); other.finish(0);
  const final = await Promise.all([observer.final, other.final]);
  check(final.every(value => value.rc === 0 && value.destroy === 0), 'native failed-join retry and complete teardown');
  console.log('SDK all_runtimes_closed=PASS');
}

run().catch(async error => {
  console.error('SDK failure', error.stack || String(error), error.cause || '');
  for (const runtime of [globalThis.Module, document.getElementById('secondary').contentWindow.Module]) {
    const fixture = runtime?.h2LocalFixture;
    if (fixture) { fixture.gate(0); fixture.finish(1); }
  }
});
