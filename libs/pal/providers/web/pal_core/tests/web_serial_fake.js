globalThis.isSecureContext = true;
globalThis.h2FakeSerialMode = 'normal';
globalThis.h2FakeCancelSettled = false;
globalThis.h2FakeCloseRejectedBeforeCancelSettled = false;

const h2FakeStorageValues = new Map();
globalThis.localStorage = {
  get length() { return h2FakeStorageValues.size; },
  key(index) { return Array.from(h2FakeStorageValues.keys())[index] ?? null; },
  getItem(key) { return h2FakeStorageValues.has(key) ? h2FakeStorageValues.get(key) : null; },
  setItem(key, value) { h2FakeStorageValues.set(String(key), String(value)); },
  removeItem(key) { h2FakeStorageValues.delete(String(key)); },
  clear() { h2FakeStorageValues.clear(); },
};

const h2FakeCanvasListeners = new Map();
Module.canvas = {
  style: {},
  addEventListener(name, callback) { h2FakeCanvasListeners.set(name, callback); },
  removeEventListener(name, callback) {
    if (h2FakeCanvasListeners.get(name) === callback) {
      h2FakeCanvasListeners.delete(name);
    }
  },
  setPointerCapture() {},
  getBoundingClientRect() { return {left: 0, top: 0, width: 2, height: 2}; },
};

const h2FakeReader = {
  read() {
    if (globalThis.h2FakeSerialMode === 'timeout-read') {
      return new Promise(resolve => {
        globalThis.h2FakePendingReadResolve = resolve;
      });
    }
    if (globalThis.h2FakeSerialMode === 'delayed-read') {
      return new Promise(resolve => setTimeout(() => resolve({
        done: false,
        value: new Uint8Array([0x48, 0x32]),
      }), 5));
    }
    if (globalThis.h2FakeSerialMode === 'unplug-read') {
      h2FakePort.connected = false;
      return Promise.reject({name: 'NetworkError'});
    }
    if (globalThis.h2FakeSerialMode === 'grow-read') {
      _emscripten_resize_heap(HEAPU8.length + 65536);
    }
    if (globalThis.h2FakeSerialMode === 'partial-read') {
      return Promise.resolve({
        done: false,
        value: new Uint8Array([0x48, 0x32, 0x21, 0x22]),
      });
    }
    return Promise.resolve({done: false, value: new Uint8Array([0x48, 0x32])});
  },
  cancel() {
    if (globalThis.h2FakePendingReadResolve) {
      globalThis.h2FakePendingReadResolve({done: true});
      globalThis.h2FakePendingReadResolve = null;
    }
    if (globalThis.h2FakeSerialMode === 'close-needs-cancel-turn') {
      return Promise.resolve().then(() => {
        globalThis.h2FakeCancelSettled = true;
      });
    }
    return Promise.resolve();
  },
  releaseLock() {},
};

const h2FakeWriter = {
  ready: Promise.resolve(),
  write(bytes) {
    globalThis.h2FakeSerialWritten = Array.from(bytes);
    if (globalThis.h2FakeSerialMode === 'timeout-write') {
      return new Promise(() => {});
    }
    if (globalThis.h2FakeSerialMode === 'unplug-write') {
      h2FakePort.connected = false;
      return Promise.reject({name: 'NetworkError'});
    }
    return Promise.resolve();
  },
  abort() { return Promise.resolve(); },
  releaseLock() {},
};

const h2FakePort = {
  connected: true,
  readable: {getReader() { return h2FakeReader; }},
  writable: {getWriter() { return h2FakeWriter; }},
  getInfo() { return {usbVendorId: 0x303a, usbProductId: 0x1001}; },
  open() {
    if (globalThis.h2FakeSerialMode === 'revoked-open') {
      return Promise.reject({name: 'NotAllowedError'});
    }
    if (globalThis.h2FakeSerialMode === 'busy-open') {
      return Promise.reject({name: 'InvalidStateError'});
    }
    return Promise.resolve();
  },
  close() {
    if (globalThis.h2FakeSerialMode === 'close-needs-cancel-turn' &&
        !globalThis.h2FakeCancelSettled) {
      globalThis.h2FakeCloseRejectedBeforeCancelSettled = true;
      return Promise.reject({name: 'InvalidStateError'});
    }
    return Promise.resolve();
  },
  setSignals(signals) {
    globalThis.h2FakeSerialSignals = signals;
    return Promise.resolve();
  },
  forget() {
    globalThis.h2FakeForgetCount = (globalThis.h2FakeForgetCount || 0) + 1;
    return Promise.resolve();
  },
};
globalThis.h2FakeSerialPort = h2FakePort;

class H2FakeAudioSource {
  connect(node) { return node; }
  start() {
    this.started = true;
    globalThis.h2FakeAudioActiveSources =
        (globalThis.h2FakeAudioActiveSources || 0) + 1;
  }
  stop() {
    if (!this.started || this.stopped) return;
    this.stopped = true;
    globalThis.h2FakeAudioActiveSources -= 1;
    globalThis.h2FakeAudioStoppedSources =
        (globalThis.h2FakeAudioStoppedSources || 0) + 1;
    if (this.onended) this.onended();
  }
}

class H2FakeAudioContext {
  constructor() {
    this.currentTime = 0;
    this.destination = {};
    this.state = 'running';
  }
  createBuffer(channels, samples, sampleRate) {
    const outputs = Array.from(
        {length: channels}, () => new Float32Array(samples));
    return {
      duration: samples / sampleRate,
      getChannelData(channel) { return outputs[channel]; },
    };
  }
  createBufferSource() { return new H2FakeAudioSource(); }
  createGain() {
    return {gain: {value: 1}, connect(node) { return node; }};
  }
  close() { return Promise.resolve(); }
  resume() { this.state = 'running'; return Promise.resolve(); }
}
globalThis.AudioContext = H2FakeAudioContext;

class H2FakeEncodedChunk {
  constructor(init) { Object.assign(this, init); }
}

class H2FakeVideoFrame {
  constructor(timestamp, duration) {
    this.displayWidth = 2;
    this.displayHeight = 2;
    this.timestamp = timestamp;
    this.duration = duration;
  }
  allocationSize() { return 16; }
  copyTo(destination) {
    destination.set([
      255, 0, 0, 255, 0, 255, 0, 255,
      0, 0, 255, 255, 255, 255, 255, 255,
    ]);
    return Promise.resolve([{offset: 0, stride: 8}]);
  }
  close() {}
}

class H2FakeVideoDecoder {
  static isConfigSupported(config) {
    return Promise.resolve({supported: config.codec.startsWith('avc1.')});
  }
  constructor(callbacks) {
    this.callbacks = callbacks;
    this.decodeQueueSize = 0;
  }
  configure() {}
  decode(chunk) {
    Promise.resolve().then(() => this.callbacks.output(
        new H2FakeVideoFrame(chunk.timestamp, chunk.duration)));
  }
  flush() { return Promise.resolve(); }
  close() {}
}

class H2FakeAudioData {
  constructor(timestamp, duration) {
    this.sampleRate = 16000;
    this.numberOfFrames = 2;
    this.numberOfChannels = 1;
    this.timestamp = timestamp;
    this.duration = duration;
  }
  allocationSize() { return 4; }
  copyTo(destination) {
    new Int16Array(destination.buffer, destination.byteOffset, 2)
        .set([1000, -1000]);
    return Promise.resolve();
  }
  close() {}
}

class H2FakeAudioDecoder {
  static isConfigSupported(config) {
    return Promise.resolve({supported: config.codec === 'mp4a.40.2'});
  }
  constructor(callbacks) {
    this.callbacks = callbacks;
    this.decodeQueueSize = 0;
  }
  configure() {}
  decode(chunk) {
    Promise.resolve().then(() => this.callbacks.output(
        new H2FakeAudioData(chunk.timestamp, chunk.duration)));
  }
  flush() { return Promise.resolve(); }
  close() {}
}

globalThis.EncodedVideoChunk = H2FakeEncodedChunk;
globalThis.EncodedAudioChunk = H2FakeEncodedChunk;
globalThis.VideoDecoder = H2FakeVideoDecoder;
globalThis.AudioDecoder = H2FakeAudioDecoder;

Object.defineProperty(globalThis, "navigator", {
  configurable: true,
  value: {
    mediaDevices: {
      getUserMedia() {
        globalThis.h2FakeGetUserMediaCount =
            (globalThis.h2FakeGetUserMediaCount || 0) + 1;
        if (globalThis.h2FakeRejectGetUserMedia) {
          return Promise.reject({name: 'NotAllowedError'});
        }
        const track = {
          kind: 'audio',
          stopped: false,
          stop() { this.stopped = true; },
        };
        return Promise.resolve({
          getTracks() { return [track]; },
          getAudioTracks() { return [track]; },
        });
      },
    },
    serial: {
      getPorts() { return Promise.resolve([h2FakePort]); },
      requestPort() {
        if (globalThis.h2FakeSerialMode === 'denied') {
          return Promise.reject({name: 'NotFoundError'});
        }
        if (globalThis.h2FakeSerialMode === 'delayed') {
          return new Promise(resolve => setTimeout(() => resolve(h2FakePort), 1));
        }
        return Promise.resolve(h2FakePort);
      },
    },
  },
});

class H2FakeRTCDataChannel {
  constructor(label, options, id) {
    this.label = label;
    this.id = options.id === undefined ? id : options.id;
    this.ordered = options.ordered !== false;
    this.maxRetransmits = options.maxRetransmits ?? null;
    this.maxPacketLifeTime = null;
    this.readyState = 'connecting';
    this.bufferedAmount = 0;
  }
  open() {
    this.readyState = 'open';
    if (this.onopen) this.onopen();
  }
  send(data) {
    if (this.readyState !== 'open') throw new Error('closed');
    const copied = typeof data === 'string' ? data : data.slice().buffer;
    Promise.resolve().then(() => {
      if (this.readyState === 'open' && this.onmessage) {
        this.onmessage({data: copied});
      }
    });
  }
  close() {
    if (this.readyState === 'closed') return;
    this.readyState = 'closed';
    if (this.onclose) this.onclose();
  }
}

class H2FakeRTCPeerConnection {
  constructor() {
    this.connectionState = 'new';
    this.signalingState = 'stable';
    this.iceGatheringState =
        globalThis.h2FakeWaitIce ? 'gathering' : 'complete';
    this.listeners = new Map();
    this.configuration = {iceServers: []};
    this.channels = [];
    this.nextChannelId = 0;
  }
  addTrack(track, stream) {
    const sender = {
      track,
      stream,
      replaceTrack(value) {
        globalThis.h2FakeDetachResolved = false;
        if (globalThis.h2FakeDetachReject) {
          globalThis.h2FakeDetachReject = false;
          return Promise.reject(new Error('replaceTrack failed'));
        }
        return new Promise(resolve => setTimeout(() => {
                             this.track = value;
                             globalThis.h2FakeDetachResolved = true;
                             resolve();
                           }, 5));
      }
    };
    this.audioSender = sender;
    return sender;
  }
  removeTrack(sender) { sender.track = null; }
  addTransceiver(kind, options) {
    globalThis.h2FakeRecvOnly =
        kind === 'audio' && options.direction === 'recvonly';
    return {sender : this.addTrack(null, null)};
  }
  getConfiguration() { return this.configuration; }
  setConfiguration(configuration) { this.configuration = configuration; }
  createDataChannel(label, options = {}) {
    const channel = new H2FakeRTCDataChannel(
        label, options, this.nextChannelId++);
    this.channels.push(channel);
    return channel;
  }
  createOffer() { return Promise.resolve({type: 'offer', sdp: 'fake-offer'}); }
  setLocalDescription(description) {
    this.localDescription = description;
    return Promise.resolve();
  }
  setRemoteDescription(description) {
    this.remoteDescription = description;
    this.connectionState = 'connected';
    if (this.onconnectionstatechange) this.onconnectionstatechange();
    for (const channel of this.channels) channel.open();
    if (this.ontrack && this.audioSender) {
      this.ontrack({
        track: {kind: 'audio'},
        streams: [{getTracks() { return []; }}],
      });
    }
    return globalThis.h2FakeRemoteDelay
               ? new Promise(resolve => setTimeout(resolve, 10))
               : Promise.resolve();
  }
  addEventListener(name, callback) { this.listeners.set(name, callback); }
  removeEventListener(name, callback) {
    if (this.listeners.get(name) === callback)
      this.listeners.delete(name);
  }
  close() {
    for (const channel of this.channels) channel.close();
    this.connectionState = 'closed';
    if (this.onconnectionstatechange) this.onconnectionstatechange();
  }
}

globalThis.RTCPeerConnection = H2FakeRTCPeerConnection;
globalThis.MediaStream = class {
  constructor(tracks) { this.tracks = tracks; }
  getTracks() { return this.tracks; }
  getAudioTracks() {
    return this.tracks.filter(track => track.kind === 'audio');
  }
};
globalThis.Audio = class {
  play() {
    globalThis.h2FakeAudioPlayCount =
        (globalThis.h2FakeAudioPlayCount || 0) + 1;
    return Promise.resolve();
  }
  pause() { this.paused = true; }
};

// The application, not the PAL, owns and registers media for these tests.
globalThis.h2FakeCreateMedia = token => {
  const track = {
    kind : 'audio',
    stopped : false,
    stop() { this.stopped = true; }
  };
  const media = {stream : new MediaStream([ track ]), audio : new Audio()};
  (Module.h2WebRtcTracks ||= new Map()).set(token, media);
  return media;
};
