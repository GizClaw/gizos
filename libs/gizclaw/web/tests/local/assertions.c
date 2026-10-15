#include "h2_web_main_thread.h"
#include <emscripten.h>
/* clang-format off */
EM_JS(void, h2_local_fixture_assertions,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], 'i32', () => {
    const standalone = window.parent === window ||
      new URLSearchParams(location.search).has('standalone');
    if (globalThis.h2WebGizClaw !== undefined) return 1;
    if (standalone) { console.log('LOCAL standalone_without_sdk=PASS'); return 0; }
    return new Promise(resolve => {
      const fixture = Module.h2LocalFixture = {finish: resolve, standalone: false};
      fixture.listener = event => {
        if (event.source !== window.parent || event.data?.type !== 'gizclaw-fixture-connect' ||
            event.ports.length !== 1 || fixture.port) return;
        window.removeEventListener('message', fixture.listener);
        fixture.port = event.ports[0];
        fixture.port.onmessage = event => {
          const message = event.data;
          if (message?.type === 'read')
            fixture.port.postMessage({type: 'result', id: message.id,
              value: Module._h2_local_fixture_state(message.field)});
          else if (message?.type === 'gate') {
            Module._h2_local_fixture_gate(message.value);
            fixture.port.postMessage({type: 'result', id: message.id, value: 0});
          } else if (message?.type === 'corrupt') {
            Module._h2_local_fixture_corrupt(message.value);
            fixture.port.postMessage({type: 'result', id: message.id, value: 0});
          } else if (message?.type === 'finish' && fixture.finish) {
            const finish = fixture.finish; fixture.finish = null; finish(message.rc);
          } else if (message?.type === 'closed' && fixture.closed) {
            const finish = fixture.closed; fixture.closed = null; finish(message.rc);
          }
        };
        fixture.port.start();
        fixture.port.postMessage({type: 'ready', origin: location.origin,
          isolated: globalThis.crossOriginIsolated, hasSDK: globalThis.h2WebGizClaw !== undefined});
      };
      window.addEventListener('message', fixture.listener);
      console.log('LOCAL fixture_listening origin=' + location.origin);
      window.parent.postMessage({type: 'gizclaw-fixture-ready'}, '*');
    });
  });
});
EM_JS(void, h2_local_fixture_closed,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], 'i32', () => {
    const fixture = Module.h2LocalFixture;
    if (!fixture?.port) return globalThis.h2WebGizClaw === undefined ? 0 : 1;
    return new Promise(resolve => {
      fixture.closed = resolve;
      fixture.port.postMessage({type: 'closed'});
    });
  });
});
EM_JS(void, h2_local_fixture_final,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ['i32','i32'], 'i32', (rc, destroy) => {
    const fixture = Module.h2LocalFixture;
    if (fixture?.port) {
      fixture.port.postMessage({type: 'final', rc, destroy});
      fixture.port.close();
    } else if (window.parent !== window) {
      window.parent.postMessage({type: 'gizclaw-fixture-final', rc, destroy}, '*');
    }
    return 0;
  });
});
/* clang-format on */
