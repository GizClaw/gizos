"""Real browser input with a saturated Runtime queue and explicit rejections."""

INPUT_FLOW = r"""
(() => {
  const mode = Number(new URLSearchParams(location.search).get('mode') || 0);
  const original = console.log;
  let state = null, calls = 0;
  const actual = [], expected = [];
  const native = Module._h2_web_application_host_button;
  Module._h2_web_application_host_button = (...args) => { calls++; return native(...args); };
  console.log = (...args) => {
    const line = args.map(String).join(' ');
    const found = line.match(/INPUT_STATE down=(\d+) up=(\d+) position=(\d+) component=(\d+) edge=([DU])/);
    if (found) {
      state = {down: Number(found[1]), up: Number(found[2]), position: Number(found[3])};
      actual.push(found[4] + found[5]);
    }
    if (line.includes('INPUT_TEST mode=')) {
      const before = calls;
      setTimeout(() => {
        if (calls !== before) console.error('INPUT_FLOW FAIL callback after cleanup');
        else console.log('INPUT_LATE_CALLBACK PASS');
      }, 30);
    }
    original.apply(console, args);
  };
  const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
  const edge = (key, down) => {
    if (mode === 0) expected.push((key === ']' ? '1' : '2') + (down ? 'D' : 'U'));
    globalThis.dispatchEvent(new KeyboardEvent(down ? 'keydown' : 'keyup', {key, bubbles: true}));
  };
  const wait = async (test, message) => {
    const end = performance.now() + 5000;
    while (!test()) {
      if (performance.now() > end) throw new Error(message + ' ' + JSON.stringify(state));
      await sleep(10);
    }
  };
  (async () => {
    try {
      edge(']', true);
      await wait(() => state?.down === 1, 'initial down');
      if (mode === 0) {
        edge(']', false); await wait(() => state?.up === 1, 'initial release');
        await sleep(50);
        const pixels = () => document.querySelector('#canvas').toDataURL();
        const initial = pixels();
        Module._input_test_pause(1);
        await wait(() => Module._input_test_paused() === 1, 'raw consumer did not pause');
        let resumed = false;
        for (let sweep = 0; sweep < 12; sweep++) {
          for (const key of [']', ']', ']', '[', '[', '[']) {
            edge(key, true); await sleep(30);
            edge(key, false); await sleep(30);
            if (!resumed && Module._input_test_saturated() === 1) {
              console.log('INPUT_SATURATION PASS accepted=16');
              Module._input_test_pause(0);
              resumed = true;
            }
          }
        }
        if (!resumed) throw new Error('raw queue never rejected an edge while all 16 slots were held');
        await wait(() => state?.down === 73 && state?.up === 73, 'lost ordered raw edges');
        if (actual.join(',') !== expected.join(',')) throw new Error('cross-button FIFO order changed');
        await sleep(700);
        if (state.position !== 1 || pixels() !== initial) throw new Error('reverse tabs did not restore frame');
      } else {
        if (mode === 6) {
          document.querySelector('[data-h2-button="forward"]').dispatchEvent(
              new PointerEvent('pointerdown', {pointerId: 7, bubbles: true}));
        }
        Module._input_test_block(mode === 4 ? 3 : 1);
        if (mode === 9) Module._input_test_health(1);
        edge(']', false);
        if (mode === 1) {
          // The release has already observed TIMEOUT. Every following edge
          // now attempts that same rejected release under BUSY.
          Module._input_test_block(2);
          for (let i = 0; i < 2; i++) {
            await sleep(30); edge(']', true);
            await sleep(30); edge(']', false);
          }
          Module._input_test_block(0);
          await wait(() => state?.down === 3 && state?.up === 3, 'release/next-press ordering lost');
        } else if (mode === 2) {
          const unbind = globalThis.h2WebApplicationHostUnbindButtons;
          globalThis.h2WebApplicationHostUnbindButtons = () => {
            const closing = unbind();
            // Resume only after teardown has begun waiting for its outbox.
            setTimeout(() => Module._input_test_block(0), 80);
            return closing;
          };
        } else if (mode === 3) {
          for (let i = 0; i < 300; i++) { edge(']', true); edge(']', false); }
        } else if (mode === 5) {
          Module._input_test_retain();
        } else if (mode === 8) {
          Module._input_test_fault();
        } else if (mode === 9) {
          await wait(() => Module._input_test_health(0) === 1, 'health Worker did not acquire Runtime');
          Module._input_test_block(0);
          console.log('INPUT_FLOW PASS');
          document.querySelector('#stop').click();
          await wait(() => Module._input_test_deactivated() === 1, 'teardown did not close Runtime admission');
          Module._input_test_health(2);
          return;
        } else if (mode === 6) {
          if (state.up !== 0) throw new Error('keyboard release ignored held pointer');
          document.querySelector('[data-h2-button="forward"]').dispatchEvent(
              new PointerEvent('pointercancel', {pointerId: 7, bubbles: true}));
          edge(']', true);
          globalThis.dispatchEvent(new Event('blur'));
          setTimeout(() => Module._input_test_block(0), 100);
          await wait(() => state?.down === 2 && state?.up === 2, 'blur/merged-source release lost');
        }
      }
      console.log('INPUT_FLOW PASS');
      if (![3, 4, 5, 8].includes(mode)) document.querySelector('#stop').click();
    } catch (error) { console.error('INPUT_FLOW FAIL ' + error); }
  })();
})();
"""
