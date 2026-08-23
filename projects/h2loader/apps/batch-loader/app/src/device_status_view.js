// Turn the raw status fields reported by a device into human-readable groups
// for the details dialog. Everything here is presentation only: unknown or
// absent fields collapse to a placeholder rather than inventing a value.

import { CAP } from "./device_actions";

export const CAPABILITY_KEYS = [
  ["status", CAP.STATUS],
  ["stage", CAP.STAGE],
  ["upgrade", CAP.UPGRADE],
  ["reboot", CAP.REBOOT],
  ["restart", CAP.RESTART],
  ["rollback", CAP.ROLLBACK],
  ["hold", 1 << 6],
  ["coredump", 1 << 7],
];

export function capabilityList(device) {
  const bits = Number.isInteger(device.capabilities) ? device.capabilities : 0;
  return CAPABILITY_KEYS.map(([key, bit]) => ({ key, enabled: (bits & bit) === bit }));
}

// What the device can boot right now: nothing, a staged package awaiting
// install, or an installed app. Drives the "can I switch to app" story.
export function appAvailability(device) {
  if (device.installedValid === 1) return "installed";
  if (device.stagedValid === 1) return "staged";
  if (device.role === "app") return "running";
  if (device.installedValid === 0 || device.stagedValid === 0) return "none";
  return "unknown";
}

export function shortChecksum(value) {
  return typeof value === "string" && value.length >= 12
    ? `${value.slice(0, 8)}…${value.slice(-4)}`
    : value || "";
}

export function statusRows(device) {
  const rows = [
    ["role", device.role],
    ["board", device.board],
    ["target", device.target],
    ["chip", device.chip],
    ["activeName", device.activeName],
    ["version", device.version],
    ["installState", device.installState],
    ["upgradePhase", device.upgradePhase],
    ["checksum", shortChecksum(device.checksum)],
    ["installedChecksum", shortChecksum(device.installedChecksum)],
    ["stagedBytes", Number.isFinite(device.stagedBytes) && device.stagedBytes > 0 ? String(device.stagedBytes) : undefined],
    ["appConfirmed", device.appConfirmed === undefined ? undefined : (device.appConfirmed === 1 ? "yes" : "no")],
  ];
  return rows.map(([key, value]) => ({
    key,
    value: value === undefined || value === null || value === "" ? null : String(value),
  }));
}
