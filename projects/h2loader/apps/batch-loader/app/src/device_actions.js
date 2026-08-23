// Which mode-transition operations a scanned device supports, from its live
// role and advertised capability bits (mirror libs/h2loader_host CAP_* flags).
// Rows without a completed status() scan have no role/capabilities and so
// support none of these actions until scanned.

export const CAP = { STATUS: 1 << 0, STAGE: 1 << 1, UPGRADE: 1 << 2, REBOOT: 1 << 3, RESTART: 1 << 4, ROLLBACK: 1 << 5 };

export function hasCap(device, bit) {
  return Number.isInteger(device.capabilities) && (device.capabilities & bit) === bit;
}

// Send package (stage): only the loader can receive a staged package.
export function canStage(device) {
  return Boolean(device.port) && device.role === "h2loader" && hasCap(device, CAP.STAGE);
}

// Switch to Loader: the app rolls back / returns to the loader.
export function canSwitchToLoader(device) {
  return Boolean(device.port) && device.role === "app" && hasCap(device, CAP.ROLLBACK);
}

// Switch to App: the loader reboots into the app; needs an installed or staged image.
export function canSwitchToApp(device) {
  return Boolean(device.port) && device.role === "h2loader" && hasCap(device, CAP.REBOOT) &&
    (device.installedValid === 1 || device.stagedValid === 1);
}

// Restart: restart the current mode (app restarts the app, loader reboots the loader).
export function canRestart(device) {
  return Boolean(device.port) && (
    (device.role === "app" && hasCap(device, CAP.RESTART)) ||
    (device.role === "h2loader" && hasCap(device, CAP.REBOOT)));
}
