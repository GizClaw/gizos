const h2CreateRunPoller = require('./run_poller.js');

const main = async () => {
  let nextTimer = 1;
  const callbacks = new Map();
  const terminal = [];
  const observations = [];
  const results = [-9, 0];
  const stop = h2CreateRunPoller({
  delayMs: 10,
  wouldBlock: -9,
  setTimer(callback) {
    const timer = nextTimer++;
    callbacks.set(timer, callback);
    return timer;
  },
  clearTimer(timer) { callbacks.delete(timer); },
  async tick() {
    observations.push(callbacks.size);
    return results.shift();
  },
  onTerminal(rc) { terminal.push(rc); },
  });

  const fireNext = async () => {
    const entry = callbacks.entries().next().value;
    if (!entry) process.exit(1);
    callbacks.delete(entry[0]);
    await entry[1]();
  };

  if (callbacks.size !== 1) process.exit(1);
  await fireNext();
  if (callbacks.size !== 1 || terminal.length !== 0) process.exit(1);
  await fireNext();
  if (callbacks.size !== 0 || terminal.length !== 1 || terminal[0] !== 0 ||
      observations.length !== 2 || observations.some(size => size !== 0)) {
    process.exit(1);
  }

  stop();
  if (callbacks.size !== 0) process.exit(1);

  let cancelledCalls = 0;
  const cancel = h2CreateRunPoller({
  delayMs: 10,
  wouldBlock: -9,
  setTimer(callback) {
    const timer = nextTimer++;
    callbacks.set(timer, callback);
    return timer;
  },
  clearTimer(timer) { callbacks.delete(timer); },
  tick() { ++cancelledCalls; return -9; },
  onTerminal() { process.exit(1); },
  });
  cancel();
  if (callbacks.size !== 0 || cancelledCalls !== 0) process.exit(1);

  const errors = [];
  h2CreateRunPoller({
    delayMs: 10,
    wouldBlock: -9,
    setTimer(callback) {
      const timer = nextTimer++;
      callbacks.set(timer, callback);
      return timer;
    },
    clearTimer(timer) { callbacks.delete(timer); },
    async tick() { throw new Error('tick failed'); },
    onTerminal() { process.exit(1); },
    onError(error) { errors.push(error.message); },
  });
  await fireNext();
  if (callbacks.size !== 0 || errors.length !== 1 ||
      errors[0] !== 'tick failed') {
    process.exit(1);
  }

  let busy = true;
  let busyTicks = 0;
  h2CreateRunPoller({
    delayMs: 10,
    wouldBlock: -9,
    isBusy() { return busy; },
    setTimer(callback) {
      const timer = nextTimer++;
      callbacks.set(timer, callback);
      return timer;
    },
    clearTimer(timer) { callbacks.delete(timer); },
    tick() { ++busyTicks; return 0; },
    onTerminal() {},
    onError() { process.exit(1); },
  });
  await fireNext();
  if (callbacks.size !== 1 || busyTicks !== 0) process.exit(1);
  busy = false;
  await fireNext();
  if (callbacks.size !== 0 || busyTicks !== 1) process.exit(1);
};

main().catch(() => process.exit(1));
