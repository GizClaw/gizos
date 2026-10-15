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
    version: 1,
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
