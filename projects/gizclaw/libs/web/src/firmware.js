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

/** Bind the optional object/Promise facade to one already running WASM module. */
export function createFirmwareClient(runtime) {
  const error = (code, details = {}) => Object.assign(
    new Error(details.rpcMessage || 'GizClaw firmware request failed (' + code + ')',
      details.cause ? {cause: details.cause} : undefined),
    {name: 'GizClawError', code, ...details});
  const signed32 = value => Number.isInteger(value) && value >= -2147483648 && value <= 2147483647;
  const bytes = value => ArrayBuffer.isView(value) &&
    Object.prototype.toString.call(value) === '[object Uint8Array]';
  if (!runtime || typeof runtime !== 'object') throw error(-1);
  const abi = runtime.h2GizClaw;
  if (!abi || abi.closed) throw error(-10);
  if (abi.version !== protocol.version) throw error(-3);
  if (typeof abi.closed !== 'boolean') throw error(-15);
  if (typeof abi.request !== 'function' || typeof abi.cancel !== 'function') throw error(-1);
  for (const [name, maximum] of [['request_capacity', 32], ['request_limit_bytes', 1048576], ['timeout_ms', 120000]])
    if (!Number.isInteger(abi[name]) || abi[name] < 1 || abi[name] > maximum) throw error(-15);
  let closed = false;
  const pending = new Set();
  const assertOpen = () => {
    if (closed || abi.closed || runtime.h2GizClaw !== abi) throw error(-10);
  };
  const cancel = (request, code = -10) => {
    if (request.done) return;
    try { abi.cancel(request.id); } catch (_) {}
    if (!request.done) { request.cleanup(); request.reject(error(code)); }
  };
  const close = () => {
    if (closed) return;
    closed = true;
    for (const request of [...pending]) cancel(request);
  };
  const call = (method, payload, opts = {}, decode = value => value) => {
    try {
      assertOpen();
      if (!signed32(method) || !bytes(payload) || !opts || typeof opts !== 'object') throw error(-1);
      if (payload.byteLength > abi.request_limit_bytes) throw error(-13);
      const timeout_ms = opts.timeout_ms ?? abi.timeout_ms;
      if (!Number.isInteger(timeout_ms) || timeout_ms < 1 || timeout_ms > 120000) throw error(-1);
      const signal = opts.signal;
      if (signal && (typeof signal.addEventListener !== 'function' ||
                     typeof signal.removeEventListener !== 'function')) throw error(-1);
      if (signal?.aborted) throw error(-10);
      payload = new Uint8Array(payload);
      return new Promise((resolve, reject) => {
        const request = {id: 0, done: false, reject};
        const abort = () => cancel(request);
        request.cleanup = () => {
          request.done = true; pending.delete(request);
          if (signal) signal.removeEventListener('abort', abort);
        };
        const response = message => {
          if (request.done) return -10;
          request.cleanup();
          let delivery = -10, value, failure;
          try {
            assertOpen();
            if (!message || !signed32(message.code) || typeof message.hasError !== 'boolean' ||
                typeof message.rpcMessage !== 'string' || !bytes(message.payload) ||
                (message.hasError && !signed32(message.rpcCode))) throw new TypeError('invalid firmware response');
            if (message.code !== 0 || message.hasError) {
              failure = error(message.code || -1000, {
                rpcCode: message.hasError ? message.rpcCode : undefined, rpcMessage: message.rpcMessage});
            } else {
              let native;
              if (message.nativeTools != null) {
                if (method !== codec.RPC_METHOD_IDS['client.tool.v0.list'] ||
                    !Array.isArray(message.nativeTools) ||
                    !message.nativeTools.every(id => Number.isInteger(id) && id >= 0 && id <= 0xffffffff))
                  throw new TypeError('invalid firmware discovery result');
                native = {tools: [...message.nativeTools]};
              }
              value = decode(new Uint8Array(message.payload), native);
              delivery = 0;
            }
          } catch (cause) {
            failure = cause?.code === -10 ? cause : error(-15, {cause});
            delivery = failure.code;
          }
          if (failure) reject(failure);
          else resolve(value);
          return delivery;
        };
        pending.add(request);
        try {
          request.id = abi.request(method, payload, response, {timeout_ms});
          if (!Number.isInteger(request.id) || request.id === 0 || request.id > 0x7fffffff || request.id < -2147483648)
            throw error(-15);
          if (request.id < 0) { request.cleanup(); reject(error(request.id)); return; }
          if (signal) {
            signal.addEventListener('abort', abort, {once: true});
            if (signal.aborted) abort();
          }
        } catch (cause) {
          if (request.id > 0) { try { abi.cancel(request.id); } catch (_) {} }
          if (!request.done) { request.cleanup(); reject(cause?.code === undefined ? error(-4, {cause}) : cause); }
        }
      });
    } catch (cause) { return Promise.reject(cause?.code === undefined ? error(-1, {cause}) : cause); }
  };
  return createGizClawSDK(codec, {call, error, assertOpen, close});
}
