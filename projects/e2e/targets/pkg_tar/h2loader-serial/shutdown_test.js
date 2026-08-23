const h2CreatePageShutdown = require('./shutdown.js');

const authorize = {disabled: false};
const run = {disabled: false};
const cancel = {disabled: false};
const result = {textContent: '', dataset: {}};
let calls = 0;
let clears = 0;
const shutdown = h2CreatePageShutdown({
  Module: {
    ccall(name) {
      if (name !== 'h2_web_h2loader_shutdown_step') process.exit(1);
      ++calls;
      return calls < 3 ? -9 : -3;
    },
  },
  authorize,
  run,
  cancel,
  result,
  clearRunTimer() { ++clears; },
});

shutdown();
shutdown();
if (!authorize.disabled || !run.disabled || !cancel.disabled ||
    calls !== 3 || clears !== 1 || result.dataset.terminal !== 'unsupported' ||
    !result.textContent.startsWith('UNSUPPORTED:')) {
  process.exit(1);
}
