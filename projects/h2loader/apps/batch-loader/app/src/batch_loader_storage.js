const STORAGE_KEY = "h2loader.batch-loader.devices.v1";

export function loadDeviceMetadata() {
  try {
    const value = JSON.parse(localStorage.getItem(STORAGE_KEY) || "[]");
    if (!Array.isArray(value)) return [];
    return value.filter((item) => item && typeof item.portId === "string").map((item) => ({
      portId: item.portId,
      name: typeof item.name === "string" ? item.name : item.portId,
      serial: typeof item.serial === "string" ? item.serial : undefined,
      board: typeof item.board === "string" ? item.board : undefined,
      target: typeof item.target === "string" ? item.target : undefined,
      role: typeof item.role === "string" ? item.role : undefined,
      version: typeof item.version === "string" ? item.version : undefined,
      usbVendorId: Number.isInteger(item.usbVendorId) ? item.usbVendorId : undefined,
      usbProductId: Number.isInteger(item.usbProductId) ? item.usbProductId : undefined,
      selected: item.selected === true,
      stale: true,
      state: "stale",
      lastResult: typeof item.lastResult === "string" ? item.lastResult : undefined,
    }));
  } catch {
    return [];
  }
}

export function saveDeviceMetadata(devices) {
  const metadata = devices.map(({ portId, name, serial, board, target, role, version, usbVendorId, usbProductId, selected, lastResult }) => ({
    portId,
    name,
    serial,
    board,
    target,
    role,
    version,
    usbVendorId,
    usbProductId,
    selected: selected === true,
    lastResult,
  }));
  localStorage.setItem(STORAGE_KEY, JSON.stringify(metadata));
}
