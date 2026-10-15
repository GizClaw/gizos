#include "h2_web_main_thread.h"
#include <emscripten.h>
/* This fixture exercises only the public JS SDK. Test controls drive its
 * entry-owned product loop; they never implement MHS or catalog operations. */
/* clang-format off */
EM_JS(void, h2_web_fixture_assertions,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ['pointer','u32','pointer','u32','i32'], 'i32',
    async (packageAddress, packageSize, readonlyAddress, readonlySize, readonlyMethod) => {
      const packageBytes = HEAPU8.slice(packageAddress, packageAddress + packageSize);
      const readonlyRequest = HEAPU8.slice(readonlyAddress, readonlyAddress + readonlySize);
      const blob = new Blob([packageBytes]);
      const read = field => Module._h2_web_fixture_state(field);
      const action = value => Module._h2_web_fixture_action(value);
      const check = (value, message) => { if (!value) throw new Error(message); };
      const wait = async (predicate, message) => {
        const deadline = performance.now() + 15000;
        while (!predicate()) {
          check(read(11) === 0, 'product worker error ' + read(11));
          if (performance.now() >= deadline) throw new Error(message);
          await new Promise(resolve => setTimeout(resolve, 5));
        }
      };
      const rejects = async (operation, code, rpcCode) => {
        try { await operation(); }
        catch (error) {
          check(error.code === code, `expected PAL ${code}, got ${error.code}: ${error}`);
          if (rpcCode !== undefined) check(error.rpcCode === rpcCode, 'wire error code ' + error.rpcCode);
          return error;
        }
        throw new Error('operation unexpectedly succeeded');
      };
      try {
        let api = globalThis.h2WebGizClaw;
        const apps = globalThis.h2WebLuaApps;
        check(api.version === 1 && apps.version === 1, 'public adapters attach');
        console.log("LOCAL_DEVICE stage=tools-list");
        const tools = await api.tools.list();
        for (const name of ['info.get','device.status.get','device.find','social.ping','lua.app.run'])
          check(tools.some(tool => tool.name === name), 'advertised real handler ' + name);
        console.log("LOCAL_DEVICE stage=info-get");
        check((await api.tools.invoke('info.get', {})).model === 'local-web-fixture', 'actual product model');
        const status = await api.tools.invoke('device.status.get', {});
        check(Object.hasOwn(status, 'battery_percent') && status.battery_percent === 0 &&
              Object.hasOwn(status, 'charging') && status.charging === false, 'zero/false facts preserve presence');
        await api.mhs.write({id:'led.fixture',hwd:'led',payload:{brightness_percent:67,enabled:true}});
        check(read(5) === 67 && read(6) === 1, 'initial mapped Runtime state changes');
        let applied = (await api.mhs.write({id:'led.fixture',hwd:'led',payload:{brightness_percent:0,enabled:false}})).payload.applied;
        check(applied.brightness_percent === 0 && applied.enabled === false && read(5) === 0 && read(6) === 0,
              'MHS writes mapped Runtime LED provider');
        let value = (await api.mhs.read({id:'led.fixture',hwd:'led'})).payload;
        check(Object.hasOwn(value, 'brightness_percent') && value.brightness_percent === 0 &&
              Object.hasOwn(value, 'enabled') && value.enabled === false, 'MHS readback preserves optional zero/false');
        const writes = read(4);
        await rejects(() => api.mhs.write({id:'led.fixture',hwd:'led',payload:{brightness_percent:101}}), -1, 3);
        check(read(4) === writes, 'invalid write never reaches Runtime');
        await rejects(() => api.mhs.read({id:'absent.fixture',hwd:'led'}), -8, 5);
        await rejects(() => api.mhs.read({id:'led.fixture',hwd:'speaker'}), -1, 3);
        await rejects(() => api.mhs.write({id:'wifi.fixture',hwd:'wifi',payload:{}}), -3);
        await rejects(() => api.call(readonlyMethod, readonlyRequest), -3, 12);
        await rejects(() => api.call(9999, new Uint8Array()), -3, 12);
        const invalidWire = await rejects(() => api.call(133, new Uint8Array([0x0a,0xff])), -1000, 3);
        check(invalidWire.name === 'GizClawError', 'typed wire error');
        await api.mhs.write({id:'speaker.main',hwd:'speaker',payload:{volume_percent:23,muted:false}});
        value = (await api.mhs.read({id:'speaker.main',hwd:'speaker'})).payload;
        check(value.volume_percent === 23 && value.muted === false, 'actual Web Audio/Runtime speaker state');
        await rejects(() => api.tools.invoke('social.ping', {from_peer_public_key:'fixture'}), -1234, 13);
        const networkError = await rejects(() => api.tools.invoke('wifi.scan', {}), -1000, 12);
        check(networkError.rpcMessage.length > 0, 'unsupported network capability remains a real failure');
        action(1);
        const slowController = new AbortController();
        const slow = api.tools.invoke('device.find', {}, {signal:slowController.signal});
        await wait(() => read(2) === 1, 'real product delegate starts');
        const reads = read(10), controllers = [], queued = [];
        for (let i = 0; i < 3; ++i) {
          const controller = new AbortController(); controllers.push(controller);
          queued.push(api.mhs.read({id:'led.fixture',hwd:'led'}, {signal:controller.signal}).then(
            () => ({ok:true}), error => ({code:error.code})));
        }
        await rejects(() => api.mhs.read({id:'led.fixture',hwd:'led'}), -18);
        for (const controller of controllers) controller.abort();
        check((await Promise.all(queued)).every(value => value.code === -10), 'all queued requests cancel');
        action(2); await slow;
        slowController.abort();
        await wait(() => read(3) === 1, 'actual delivery completion runs exactly once');
        check(read(10) === reads && read(14) === 0, 'canceled queued calls never invoke MHS; actual completion succeeds');
        console.log('LOCAL_DEVICE sdk=PASS');

        await apps.install(blob);
        check((await apps.list()).find(app => app.app_id === 'demo').version === '0.2.0', 'one installed overlay catalog');
        let old = api;
        action(7);
        await api.tools.invoke('lua.app.run', {app_id:'demo',params:{kind:'natural'}});
        await wait(() => read(1) === 4, 'failed bridge join retains handoff');
        await rejects(() => old.mhs.read({id:'led.fixture',hwd:'led'}), -10);
        await rejects(() => apps.uninstall('demo'), -18);
        action(6);
        await wait(() => read(1) === 2 && read(7) === 1, 'real Lua starts with independent KV');
        await rejects(() => apps.install(blob), -18);
        await rejects(() => apps.uninstall('demo'), -18);
        check((await apps.list()).find(app => app.app_id === 'demo').version === '0.2.0', 'pin preserves active entry');
        action(3);
        await wait(() => read(1) === 1 && read(12) === 1, 'natural exit destroys Host and restores Main');
        api = globalThis.h2WebGizClaw;
        check(api !== old, 'Main rebinds a fresh SDK');
        await rejects(() => old.tools.list(), -10);
        check((await api.mhs.read({id:'led.fixture',hwd:'led'})).payload.brightness_percent === 0,
              'offline MHS userdata survives Service creation/rebind');
        console.log('LOCAL_DEVICE lua_natural=PASS');
        console.log('LOCAL_DEVICE close_retry=PASS');

        await apps.uninstall('demo');
        check((await apps.list()).length === 0, 'uninstall persists preinstalled hiding');
        await apps.install(blob);
        old = api;
        action(8);
        await api.tools.invoke('lua.app.run', {app_id:'demo',params:{kind:'stop'}});
        await wait(() => read(1) === 2 && read(7) === 2, 'real Lua KV survives uninstall/reinstall');
        action(4);
        await wait(() => read(1) === 3, 'failed Lua join retains Host after job cancellation');
        check(read(13) === 1, 'actual Lua job reached CANCELLED');
        await rejects(() => apps.uninstall('demo'), -18);
        await rejects(() => apps.install(blob), -18);
        action(5);
        await wait(() => read(1) === 1 && read(8) === 2, 'checked Host destruction releases lease and restores Main');
        await rejects(() => old.tools.list(), -10);
        api = globalThis.h2WebGizClaw;
        console.log("LOCAL_DEVICE stage=info-get");
        check((await api.tools.invoke('info.get', {})).model === 'local-web-fixture', 'product delegate remains live after repeated handoff');
        await apps.uninstall('demo');
        check((await apps.list()).length === 0 && read(9) === 2, 'lease releases only after actual checked cleanup');
        console.log('LOCAL_DEVICE lua_stop_retry=PASS');
        return 0;
      } catch (error) {
        console.error('LOCAL_DEVICE failure', JSON.stringify({code:error.code,rpcCode:error.rpcCode,phase:read(1),worker:read(11)}), error.stack || String(error), error.cause?.stack || error.cause || "");
        action(2);
        return 1;
      }
    });
});
/* clang-format on */
