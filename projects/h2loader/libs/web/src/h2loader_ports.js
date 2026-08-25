export function createH2LoaderPortRegistry() {
  const portIds = new WeakMap();
  const portObjects = new Map();

  const wrap = (entry) => {
    const existing = portObjects.get(entry.id);
    if (existing) return existing;
    const port = Object.freeze({
      id: entry.id,
      label: entry.label || "Authorized serial device",
      usbVendorId: entry.usbVendorId || 0,
      usbProductId: entry.usbProductId || 0,
    });
    portIds.set(port, entry.id);
    portObjects.set(entry.id, port);
    return port;
  };

  return Object.freeze({
    clear() {
      portObjects.clear();
    },
    delete(port) {
      const id = portIds.get(port);
      if (id) portObjects.delete(id);
      portIds.delete(port);
    },
    id(port) {
      return portIds.get(port);
    },
    wrap,
  });
}
