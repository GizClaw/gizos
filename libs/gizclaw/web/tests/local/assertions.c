#include "h2_web_main_thread.h"
#include <emscripten.h>
/* clang-format off */
EM_JS(void, h2_local_fixture_assertions,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], 'i32', () => {
    const abi = Module.h2GizClaw;
    if (!abi || abi.version !== 1 || globalThis.h2WebGizClaw !== undefined) return 1;
    const controlled = globalThis.h2LocalSDKTest || new URLSearchParams(location.search).has('sdk-test');
    if (!controlled) {
      // Actual raw callback invocation without loading the npm SDK or a codec.
      return new Promise(resolve => {
        let returned = false, count = 0;
        const id = abi.request(136, new Uint8Array(), response => {
          const valid = returned && ++count === 1 && response.code === 0 && !response.hasError &&
            response.payload instanceof Uint8Array && response.nativeTools?.length > 0;
          if (valid) console.log('LOCAL standalone_without_sdk=PASS');
          resolve(valid ? 0 : 1);
          return valid ? 0 : -15;
        });
        returned = true;
        if (id < 1) resolve(1);
      });
    }
    return new Promise(resolve => {
      let final;
      Module.h2LocalFixture = {
        finish: resolve,
        read: field => Module._h2_local_fixture_state(field),
        gate: value => Module._h2_local_fixture_gate(value),
        corrupt: value => Module._h2_local_fixture_corrupt(value),
        final: new Promise(done => { final = done; }),
        complete: value => final(value),
      };
    });
  });
});
EM_JS(void, h2_local_fixture_closed,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], 'i32', () => {
    if (Module.h2GizClaw !== null) return 1;
    return Module.h2LocalFixture?.onClosed?.() ?? 0;
  });
});
EM_JS(void, h2_local_fixture_final,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ['i32','i32'], 'i32', (rc, destroy) => {
    Module.h2LocalFixture?.complete({rc, destroy});
    return 0;
  });
});
/* clang-format on */
