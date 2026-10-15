#include "h2_web_main_thread.h"
#include <emscripten.h>
/* clang-format off */
EM_JS(void, h2_local_fixture_assertions,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], 'i32', async () => {
    const check = (value, message) => { if (!value) throw new Error(message); };
    const read = field => Module._h2_local_fixture_state(field);
    const rejects = async (operation, code, rpcCode) => {
      try { await operation(); } catch (error) {
        check(error.code === code, `expected ${code}, got ${error.code}`);
        if (rpcCode !== undefined) check(error.rpcCode === rpcCode, 'wire code');
        return;
      }
      throw new Error('operation unexpectedly succeeded');
    };
    const wait = async predicate => {
      const deadline = performance.now() + 15000;
      while (!predicate()) {
        check(performance.now() < deadline, 'owner progress deadline');
        await new Promise(resolve => setTimeout(resolve, 5));
      }
    };
    try {
      const api = globalThis.h2WebGizClaw;
      check(api.version === 1, 'bind');
      const tools = await api.tools.list();
      for (const name of ['info.get','device.find','social.ping','lua.app.list','lua.app.install','lua.app.uninstall'])
        check(tools.some(tool => tool.name === name), 'existing tool ' + name);
      check((await api.tools.invoke('info.get', {})).model === 'local-fixture', 'existing info handler');
      await api.mhs.write({id:'led.fixture',hwd:'led',payload:{brightness_percent:67,enabled:true}});
      check(read(6) === 67 && read(7) === 1, 'registered MHS handler');
      let value = (await api.mhs.write({id:'led.fixture',hwd:'led',payload:{brightness_percent:0,enabled:false}})).payload.applied;
      check(value.brightness_percent === 0 && value.enabled === false, 'applied zero/false');
      value = (await api.mhs.read({id:'led.fixture',hwd:'led'})).payload;
      check(Object.hasOwn(value,'brightness_percent') && value.brightness_percent === 0 &&
            Object.hasOwn(value,'enabled') && value.enabled === false, 'presence');
      const writes = read(2);
      await rejects(() => api.mhs.write({id:'led.fixture',hwd:'led',payload:{brightness_percent:101}}), -1, 3);
      check(read(2) === writes, 'invalid input never invokes provider');
      await rejects(() => api.mhs.read({id:'missing.fixture',hwd:'led'}), -8, 5);
      await rejects(() => api.tools.invoke('social.ping', {from_peer_public_key:'fixture'}), -1234, 13);
      await rejects(() => api.call(9999, new Uint8Array()), -3, 12);
      await rejects(() => api.call(133, new Uint8Array([0x0a,0xff])), -1000, 3);
      check((await api.tools.invoke('lua.app.list', {})).apps.length === 0, 'existing empty catalog');
      const installed = await api.tools.invoke('lua.app.install', {url:'https://fixture.invalid/demo'});
      check(installed.app.app_id === 'demo' && installed.app.version === '0.2.0', 'existing installer');
      check((await api.tools.invoke('lua.app.list', {})).apps[0].version === '0.2.0', 'existing list');
      await api.tools.invoke('lua.app.uninstall', {app_id:'demo'});
      check((await api.tools.invoke('lua.app.list', {})).apps.length === 0, 'existing uninstall');
      console.log('LOCAL existing_lua_tools=PASS');
      Module._h2_local_fixture_gate(1);
      const slow = api.tools.invoke('device.find', {});
      await wait(() => read(3) === 1);
      const reads = read(1), controllers = [], pending = [];
      for (let i = 0; i < 3; ++i) {
        const controller = new AbortController(); controllers.push(controller);
        pending.push(api.mhs.read({id:'led.fixture',hwd:'led'}, {signal:controller.signal}).then(
          () => 0, error => error.code));
      }
      await rejects(() => api.mhs.read({id:'led.fixture',hwd:'led'}), -18);
      controllers.forEach(controller => controller.abort());
      check((await Promise.all(pending)).every(code => code === -10), 'queued cancellation');
      Module._h2_local_fixture_gate(0); await slow;
      await wait(() => read(4) === 1);
      check(read(1) === reads && read(5) === 0, 'no canceled provider call; one completion');
      console.log('LOCAL tools_mhs=PASS');
      return 0;
    } catch (error) {
      Module._h2_local_fixture_gate(0);
      console.error('LOCAL failure', error.stack || String(error), error.cause || "");
      return 1;
    }
  });
});
EM_JS(void, h2_local_fixture_closed,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], 'i32', async () => {
    try { await globalThis.h2WebGizClaw.tools.list(); }
    catch (error) { return error.code === -10 ? 0 : 1; }
    return 1;
  });
});
/* clang-format on */
