// Deterministic navigator.onLine and window online/offline events for Node.
(() => {
  const listeners = {online: new Set(), offline: new Set()};
  let online = true;
  let present = true;
  Object.defineProperty(globalThis, 'navigator', {
    configurable: true,
    value: {
      get onLine() { return present ? online : undefined; },
    },
  });
  globalThis.addEventListener = (type, callback) => listeners[type]?.add(callback);
  globalThis.removeEventListener = (type, callback) =>
      listeners[type]?.delete(callback);
  globalThis.h2TestNetif = {
    set(value, notify = true) {
      online = !!value;
      if (!notify) return;
      for (const callback of [...listeners[online ? 'online' : 'offline']]) {
        callback();
      }
    },
    present(value) { present = !!value; },
    listeners() { return listeners.online.size + listeners.offline.size; },
  };
})();
