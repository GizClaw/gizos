(function(root, factory) {
  const create = factory();
  if (typeof module === 'object' && module.exports) module.exports = create;
  root.h2CreateRunPoller = create;
})(typeof globalThis !== 'undefined' ? globalThis : this, function() {
  return function h2CreateRunPoller(options) {
    let timer = 0;
    let stopped = false;

    const schedule = () => {
      timer = options.setTimer(run, options.delayMs);
    };
    const run = async () => {
      timer = 0;
      if (stopped) return;
      const asyncifyBusy = options.isBusy
        ? options.isBusy()
        : typeof Asyncify !== 'undefined' && Asyncify.currData;
      if (asyncifyBusy) {
        schedule();
        return;
      }
      let rc;
      try {
        rc = await options.tick();
      } catch (error) {
        stopped = true;
        options.onError(error);
        return;
      }
      if (rc === options.wouldBlock) {
        schedule();
        return;
      }
      stopped = true;
      options.onTerminal(rc);
    };

    schedule();
    return () => {
      stopped = true;
      if (timer !== 0) {
        options.clearTimer(timer);
        timer = 0;
      }
    };
  };
});
