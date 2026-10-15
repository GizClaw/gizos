function createGizClawSDK(codec, transport) {
  const invalid = cause => transport.error(-1, {cause});
  const encode = operation => {
    transport.assertOpen();
    try { return operation(); }
    catch (cause) { throw cause?.code === undefined ? invalid(cause) : cause; }
  };
  const id = (value, ids) => typeof value === 'string' ? ids[value] : value;
  const rpc = (method, request, options, decode) => {
    const payload = encode(() => codec.encodeRPCRequestPayload(method, request));
    return transport.call(codec.RPC_METHOD_IDS[method], payload, options, (bytes, native) => {
      const value = native ?? codec.decodeRPCResponsePayload(method, bytes);
      return decode ? decode(value) : value;
    });
  };
  const api = {
    version: protocol.version,
    close: transport.close,
    call: (method, payload, options) => transport.call(method, payload, options, bytes => bytes),
    mhs: Object.freeze({
      read: async (request, options) => {
        const hwd = encode(() => {
          const value = id(request?.hwd, codec.CLIENT_HWD_IDS);
          if (!Number.isInteger(value)) throw new TypeError('unknown HWD');
          return value;
        });
        return rpc('client.mhs.v0.read', {...request, hwd}, options, value => ({
          ...value,
          payload: codec.decodeClientHwdReadResponsePayload(hwd, codec.decodeBase64(value.payload)),
        }));
      },
      write: async (request, options) => {
        const [hwd, payload] = encode(() => {
          const value = id(request?.hwd, codec.CLIENT_HWD_IDS);
          if (!Number.isInteger(value)) throw new TypeError('unknown HWD');
          if (codec.CLIENT_HWD_NAMES[value] && !codec.hasClientHwdWriteRequest(value)) throw transport.error(-3);
          return [value, codec.encodeBase64(codec.encodeClientHwdWriteRequestPayload(value, request.payload))];
        });
        return rpc('client.mhs.v0.write', {...request, hwd, payload}, options, value => ({
          ...value,
          payload: codec.decodeClientHwdWriteResponsePayload(hwd, codec.decodeBase64(value.payload)),
        }));
      },
    }),
    tools: Object.freeze({
      list: async options => rpc('client.tool.v0.list', {}, options, value =>
        value.tools.filter(tool => codec.CLIENT_TOOL_NAMES[tool]).map(tool =>
          ({id: tool, name: codec.CLIENT_TOOL_NAMES[tool]}))),
      invoke: async (tool, request = {}, options) => {
        const [value, payload] = encode(() => {
          const value = id(tool, codec.CLIENT_TOOL_IDS);
          if (!Number.isInteger(value)) throw new TypeError('unknown tool');
          return [value, codec.encodeBase64(codec.encodeClientToolRequestPayload(value, request))];
        });
        return rpc('client.tool.v0.invoke', {tool: value, payload}, options, response =>
          codec.decodeClientToolResponsePayload(value, codec.decodeBase64(response.payload || "")));
      },
    }),
  };
  return Object.freeze(api);
}

/** Connect an optional controller to an existing firmware iframe. */
export async function connectFirmware(iframe, options = {}) {
  const error = (code, details = {}) => Object.assign(
    new Error(details.rpcMessage || 'GizClaw firmware request failed (' + code + ')',
      details.cause ? {cause: details.cause} : undefined),
    {name: 'GizClawError', code, ...details});
  const signed32 = value => Number.isInteger(value) && value >= -2147483648 && value <= 2147483647;
  const browser = globalThis.window;
  if (!browser || !iframe || iframe.tagName !== 'IFRAME' ||
      !iframe.contentWindow || typeof iframe.addEventListener !== 'function' ||
      !options || typeof options !== 'object') throw error(-1);
  const timeout = options.timeout_ms ?? 30000;
  if (!Number.isInteger(timeout) || timeout < 1 || timeout > 120000) throw error(-1);
  let origin;
  try {
    origin = options.origin ?? new URL(iframe.src, browser.document.baseURI).origin;
    if (typeof origin !== 'string' || origin === 'null' || new URL(origin).origin !== origin)
      throw new TypeError('exact non-opaque origin required');
  } catch (cause) { throw error(-1, {cause}); }
  const target = iframe.contentWindow;
  const key = crypto.randomUUID();
  return new Promise((resolve, reject) => {
    let port, retry, timer, connected = false, closed = false, serial = 0;
    const pending = new Map();
    const post = message => { if (port) port.postMessage(message); };
    const cleanHandshake = () => {
      clearTimeout(retry); clearTimeout(timer);
      browser.removeEventListener('message', available);
    };
    const close = (code = -10) => {
      if (closed) return;
      closed = true;
      cleanHandshake();
      iframe.removeEventListener('load', navigated);
      browser.removeEventListener('pagehide', pagehide);
      try { post({type: 'disconnect'}); } catch (_) {}
      if (port) { port.onmessage = null; port.onmessageerror = null; port.close(); }
      for (const request of pending.values()) { request.cleanup(); request.reject(error(code)); }
      pending.clear();
      if (!connected) reject(error(code));
    };
    const pagehide = () => close();
    const navigated = () => { if (connected) close(); };
    const assertOpen = () => { if (closed) throw error(-10); };
    const call = (method, bytes, opts = {}, decode = value => value) => {
      try {
        assertOpen();
        if (!Number.isInteger(method) || method < -2147483648 || method > 2147483647 ||
            !(bytes instanceof Uint8Array) || !opts || typeof opts !== 'object') throw error(-1);
        if (bytes.byteLength > limits.request_limit_bytes) throw error(-13);
        const deadline = opts.timeout_ms ?? limits.timeout_ms;
        if (!Number.isInteger(deadline) || deadline < 1 || deadline > 120000) throw error(-1);
        const signal = opts.signal;
        if (signal && (typeof signal.addEventListener !== 'function' ||
                       typeof signal.removeEventListener !== 'function')) throw error(-1);
        if (signal?.aborted) throw error(-10);
        if (pending.size >= limits.request_capacity) throw error(-18);
        bytes = new Uint8Array(bytes);
        do { serial = serial % 0x7fffffff + 1; } while (pending.has(serial));
        const id = serial;
        return new Promise((done, fail) => {
          let abort, expiry;
          const cleanup = () => {
            clearTimeout(expiry);
            if (signal) signal.removeEventListener('abort', abort);
          };
          const cancel = code => {
            if (!pending.has(id)) return;
            pending.delete(id); cleanup();
            try { post({type: 'cancel', id}); } catch (_) {}
            fail(error(code));
          };
          abort = () => cancel(-10);
          pending.set(id, {resolve: done, reject: fail, decode, cleanup, method});
          try {
            if (signal) signal.addEventListener('abort', abort, {once: true});
            expiry = setTimeout(() => cancel(-6), deadline);
            port.postMessage({type: 'request', id, method, payload: bytes, timeout_ms: deadline}, [bytes.buffer]);
          } catch (cause) {
            pending.delete(id); cleanup(); fail(error(-4, {cause}));
          }
        });
      } catch (cause) {
        return Promise.reject(cause?.code === undefined ? error(-1, {cause}) : cause);
      }
    };
    let limits;
    const response = message => {
      if (!message || typeof message !== 'object') { close(-15); return; }
      if (message.type === 'closed') { close(signed32(message.code) ? message.code : -15); return; }
      if (message.type !== 'response' || !Number.isInteger(message.id) || message.id < 1 || message.id > 0x7fffffff) { close(-15); return; }
      const request = pending.get(message.id);
      if (!request) { try { post({type: 'complete', id: message.id, result: -10}); } catch (_) {} return; }
      pending.delete(message.id); request.cleanup();
      let delivery = -10, value, failure;
      try {
        if (!signed32(message.code) || typeof message.hasError !== 'boolean' ||
            typeof message.rpcMessage !== 'string' || !(message.payload instanceof Uint8Array) ||
            (message.hasError && !signed32(message.rpcCode))) throw new TypeError('invalid firmware response');
        if (message.code !== 0 || message.hasError) {
          failure = error(message.code || -1000, {
            rpcCode: message.hasError ? message.rpcCode : undefined, rpcMessage: message.rpcMessage});
        } else {
          let native;
          if (message.nativeTools !== null) {
            if (request.method !== codec.RPC_METHOD_IDS['client.tool.v0.list'] ||
                !Array.isArray(message.nativeTools) ||
                !message.nativeTools.every(id => Number.isInteger(id) && id >= 0 && id <= 0xffffffff))
              throw new TypeError('invalid firmware discovery result');
            native = {tools: message.nativeTools};
          }
          value = request.decode(new Uint8Array(message.payload), native);
          delivery = 0;
        }
      } catch (cause) { delivery = -15; failure = error(-15, {cause}); }
      try { post({type: 'complete', id: message.id, result: delivery}); }
      catch (cause) { failure = error(-10, {cause}); close(); }
      if (failure) request.reject(failure);
      else request.resolve(value);
    };
    const ready = message => {
      if (!message || message.type !== 'ready' || message.key !== key ||
          message.version !== protocol.version) { close(-3); return; }
      for (const [name, maximum] of [['request_capacity', 32], ['request_limit_bytes', 1048576], ['timeout_ms', 120000]])
        if (!Number.isInteger(message[name]) || message[name] < 1 || message[name] > maximum) { close(-15); return; }
      limits = message;
      connected = true; cleanHandshake();
      port.onmessage = event => response(event.data);
      resolve(createGizClawSDK(codec, {call, error, assertOpen, close: () => close()}));
    };
    const available = event => {
      const message = event.data;
      if (closed || port || event.source !== target || event.origin !== origin ||
          !message || message.channel !== protocol.channel || message.key !== key ||
          message.type !== 'available') return;
      if (message.version !== protocol.version) { close(-3); return; }
      const channel = new MessageChannel();
      port = channel.port1;
      port.onmessage = event => ready(event.data);
      port.onmessageerror = () => close(-15);
      port.start();
      try { target.postMessage({channel: protocol.channel, version: protocol.version,
        type: 'connect', key}, origin, [channel.port2]); }
      catch (_) { channel.port2.close(); close(-4); }
    };
    const probe = () => {
      if (closed || port) return;
      try { target.postMessage({channel: protocol.channel, version: protocol.version, type: 'probe', key}, origin); }
      catch (_) { close(-4); return; }
      retry = setTimeout(probe, 50);
    };
    browser.addEventListener('message', available);
    browser.addEventListener('pagehide', pagehide);
    iframe.addEventListener('load', navigated);
    timer = setTimeout(() => close(-6), timeout);
    probe();
  });
}
