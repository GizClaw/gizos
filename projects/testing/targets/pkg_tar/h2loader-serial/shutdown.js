globalThis.h2CreatePageShutdown = ({
  Module,
  authorize,
  run,
  cancel,
  result,
  clearRunTimer,
}) => {
  let shuttingDown = false;
  return () => {
    if (shuttingDown) return;
    shuttingDown = true;
    authorize.disabled = true;
    run.disabled = true;
    cancel.disabled = true;
    clearRunTimer();
    let rc = -9;
    for (let turn = 0; turn < 1024 && rc === -9; ++turn) {
      rc = Module.ccall(
        'h2_web_h2loader_shutdown_step', 'number', [], []);
    }
    if (rc === -9) {
      result.textContent = 'Shutdown did not unwind within 1024 turns';
      result.dataset.terminal = 'fail';
    } else if (rc === -3) {
      result.textContent =
        'UNSUPPORTED: active Web Serial close cannot complete synchronously';
      result.dataset.terminal = 'unsupported';
    }
  };
};

if (typeof module !== 'undefined') {
  module.exports = globalThis.h2CreatePageShutdown;
}
