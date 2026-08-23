// Identity and rebinding rules for authorized Web Serial ports.
//
// Two physically identical devices (same USB VID/PID, no serial number exposed
// by Web Serial) are indistinguishable to the page across reloads. The only
// reliable identity is the live SerialPort object during one page session, so:
//
//   1. A live port already shown in a row keeps that row untouched (object
//      identity match) — re-enumeration when another port is authorized never
//      disturbs an existing row.
//   2. Every remaining live port consumes at most one persisted row, and each
//      persisted row is consumed at most once (strict one-to-one), so labels
//      can never collapse onto a single row or bleed across rows.
//   3. Persisted rows with no live port stay offline.

const runtimePortIds = new WeakMap();
const runtimeSessionId = globalThis.crypto?.randomUUID?.() || `${Date.now()}-${Math.random()}`;
let runtimePortSequence = 0;

// The SDK keeps its port handle private, so identity comes from the live port
// object: the SDK returns the same frozen object for a port across calls, which
// makes this WeakMap stable for the whole page session.
export function runtimePortId(port) {
  let id = runtimePortIds.get(port);
  if (!id) {
    id = `runtime:${runtimeSessionId}:${++runtimePortSequence}`;
    runtimePortIds.set(port, id);
  }
  return id;
}

function portNumber(port, index) {
  const match = /(\d+)\s*$/.exec(runtimePortId(port));
  return match ? Number(match[1]) : index + 1;
}

function buildRow(port, saved, index, defaultName) {
  const restored = Boolean(saved);
  const base = saved || {};
  return {
    ...base,
    port,
    portId: base.portId || runtimePortId(port),
    name: base.name || defaultName(portNumber(port, index)),
    usbVendorId: port.usbVendorId,
    usbProductId: port.usbProductId,
    selected: base.selected ?? true,
    state: restored ? "stale" : "ready",
    stale: restored,
    progress: 0,
    error: undefined,
  };
}

function deviceOrder(row) {
  const match = /(\d+)\s*$/.exec(typeof row.portId === "string" ? row.portId : "");
  return match ? Number(match[1]) : Number.MAX_SAFE_INTEGER;
}

export function mergeAuthorizedPorts(ports, previous, defaultName) {
  const consumed = new Set();

  // Pass 1: keep rows already bound to a still-present live port.
  const rows = ports.map((port) => {
    const bound = previous.find((item) =>
      !consumed.has(item) &&
      (item.port === port || (item.port && item.portId === runtimePortId(port))));
    if (!bound) return null;
    consumed.add(bound);
    // Same live object: preserve the row's scanned state; only refresh IDs.
    return { ...bound, usbVendorId: port.usbVendorId, usbProductId: port.usbProductId };
  });

  // Pool of persisted rows still available for a fresh live port, in order.
  const pool = previous.filter((item) => !consumed.has(item));
  const takeFromPool = (port) => {
    let index = pool.findIndex((item) =>
      item.usbVendorId === port.usbVendorId && item.usbProductId === port.usbProductId);
    if (index < 0) index = pool.findIndex((item) => item.usbVendorId === undefined);
    if (index < 0) return undefined;
    const [saved] = pool.splice(index, 1);
    consumed.add(saved);
    return saved;
  };

  // Pass 2: assign each still-unbound live port one persisted row (or a fresh row).
  const live = rows.map((row, index) =>
    row || buildRow(ports[index], takeFromPool(ports[index]), index, defaultName));

  // Persisted rows never claimed by a live port render as offline history.
  const offline = previous
    .filter((item) => !consumed.has(item))
    .map((item) => ({ ...item, port: undefined, stale: true, state: "offline", selected: false }));

  return [...live, ...offline]
    .map((row, index) => ({ row, index }))
    .sort((a, b) => (deviceOrder(a.row) - deviceOrder(b.row)) || (a.index - b.index))
    .map((entry) => entry.row);
}
