"""Require fresh periodic Lua progress after each acknowledged key release."""

LUA_PROGRESS_FLOW = r"""
(() => {
  let ticks = 0, downs = 0, ups = 0;
  const original = console.info;
  console.info = (...args) => {
    const line = args.map(String).join(' ');
    if (line.includes('LUA_PROGRESS tick=')) ticks++;
    if (line.includes('LUA_PROGRESS down=')) downs++;
    if (line.includes('LUA_PROGRESS up=')) ups++;
    original.apply(console, args);
  };
  const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
  const wait = async (predicate, message) => {
    const deadline = performance.now() + 5000;
    while (!predicate()) {
      if (performance.now() >= deadline) throw new Error(message);
      await sleep(10);
    }
  };
  (async () => {
    try {
      for (let i = 0; i < 20; i++) {
        const beforeDown = downs, beforeUp = ups;
        globalThis.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter', bubbles: true}));
        await wait(() => downs > beforeDown, 'down not handled');
        await sleep(60);
        globalThis.dispatchEvent(new KeyboardEvent('keyup', {key: 'Enter', bubbles: true}));
        await wait(() => ups > beforeUp, 'up not handled');
        const afterRelease = ticks;
        await wait(() => ticks >= afterRelease + 3, 'periodic Lua progress stopped after release');
      }
      console.log('LUA_PROGRESS PASS');
      document.querySelector('#stop').click();
    } catch (error) { console.error('LUA_PROGRESS FAIL ' + error); }
  })();
})();
"""
