// Deterministic devices and rendering for lifecycle/pressure tests. The same C
// test also runs without these substitutes in the real Chromium test target.
Module.h2MicFake = true;
class H2MicPort {
  postMessage(data) { this.other.onmessage?.({data}); }
  close() { this.onmessage = null; }
}
globalThis.AudioWorkletProcessor = class {
  constructor() { this.port = new H2MicPort(); }
};
globalThis.registerProcessor = (name, implementation) => {
  globalThis.h2MicProcessor = implementation;
};
globalThis.AudioWorkletNode = class {
  constructor(context) {
    this.processor = new globalThis.h2MicProcessor();
    this.port = new H2MicPort();
    this.port.other = this.processor.port;
    this.processor.port.other = this.port;
    context.node = this;
    this.context = context;
  }
  connect() {
    this.interval = setInterval(() => {
      if (this.context.source?.connected)
        this.processor.process([[new Float32Array(128).fill(0.25)]]);
    }, 1);
  }
  disconnect() { clearInterval(this.interval); }
};
globalThis.AudioContext = class {
  constructor({sampleRate}) {
    this.sampleRate = sampleRate;
    this.state = 'suspended';
    this.destination = {};
    this.audioWorklet = {
      addModule: async url => {
        const code = await (await fetch(url)).text();
        (0, eval)(code);
      },
    };
  }
  createMediaStreamSource(stream) {
    this.source = {
      connected: false,
      connect() { this.connected = true; },
      disconnect() { this.connected = false; },
    };
    return this.source;
  }
  async resume() { this.state = 'running'; }
  async close() { this.state = 'closed'; }
};
Object.defineProperty(globalThis, 'navigator', {configurable: true, value: {
  mediaDevices: {
    getUserMedia(constraints) {
      Module.h2MicConstraints = constraints;
      if (Module.h2MicDenied)
        return Promise.reject({name: 'NotAllowedError'});
      const listeners = new Map();
      const track = {
        readyState: 'live',
        getSettings() { return {echoCancellation: !Module.h2MicEchoDisabled}; },
        addEventListener(name, callback) { listeners.set(name, callback); },
        removeEventListener(name) { listeners.delete(name); },
        stop() { this.readyState = 'ended'; },
        end() { this.readyState = 'ended'; listeners.get('ended')?.(); },
      };
      const stream = {getTracks: () => [track], getAudioTracks: () => [track]};
      Module.h2MicLastStream = stream;
      if (Module.h2MicPending)
        return new Promise(resolve => { Module.h2MicResolve = () => resolve(stream); });
      return Promise.resolve(stream);
    },
  },
}});
