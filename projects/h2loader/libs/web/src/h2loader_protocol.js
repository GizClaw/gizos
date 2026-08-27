export const H2LoaderCapabilities = Object.freeze({
  UART: 1 << 0,
  WIFI: 1 << 1,
  BLE: 1 << 2,
  ALL: 0x00000007,
});

export const H2LoaderCommands = Object.freeze({
  REBOOT_APP: 1 << 0,
  REBOOT_LOADER: 1 << 1,
  HELP: 1 << 2,
  STATUS: 1 << 3,
  STATS: 1 << 4,
  MEMORY: 1 << 5,
  APP_RESTART: 1 << 6,
  APP_ROLLBACK: 1 << 7,
  COREDUMP_STATUS: 1 << 8,
  COREDUMP_DUMP: 1 << 9,
  COREDUMP_ERASE: 1 << 10,
  STAGE_PAYLOAD: 1 << 11,
  STAGE_ABORT: 1 << 12,
  STAGE_URL: 1 << 13,
  HOLD_ON: 1 << 14,
  HOLD_OFF: 1 << 15,
  WIFI_SCAN: 1 << 16,
  WIFI_CONNECT: 1 << 17,
  WIFI_DISCONNECT: 1 << 18,
  LOADER_UPGRADE: 1 << 19,
  ALL: 0x000fffff,
});

const roleNames = ["unknown", "h2loader", "app"];
const intentNames = ["unknown", "h2loader", "app"];
const installNames = [
  "unknown", "idle", "staged", "install-requested", "installing",
  "installed-pending-confirm", "confirmed", "install-failed",
  "return-requested", "main-failed",
];
const upgradeNames = [
  "unknown", "idle", "trial-pending", "trial-running",
  "canonical-pending", "failed", "corrupt",
];
const mfgModeNames = ["unknown", "disabled", "enabled"];
const field = (value, shift, mask) => Number((value >> BigInt(shift)) & BigInt(mask));

export function decodeH2LoaderStates(states) {
  if (typeof states !== "string" || !/^0x[0-9a-f]{16}$/.test(states)) {
    throw new TypeError("states must be 0x followed by 16 lowercase hex digits");
  }
  const value = BigInt(states);
  const role = field(value, 0, 0x3);
  const bootIntent = field(value, 2, 0x3);
  const installState = field(value, 4, 0xf);
  const upgradePhase = field(value, 8, 0x7);
  const mfgMode = field(value, 16, 0x3);
  const flagsKnown = (value & (1n << 11n)) !== 0n;
  const mfgSteps = Object.freeze(Array.from(
    {length: 22}, (_, index) => field(value, 18 + index * 2, 0x3)));
  const lifecycle = value & (0xfn << 12n);
  if ((value & (0x3n << 62n)) !== 0n || role === 0 || role === 3 ||
      bootIntent === 3 ||
      installState > 9 || upgradePhase > 6 || mfgMode === 0 || mfgMode === 3 ||
      (!flagsKnown && lifecycle !== 0n) ||
      (mfgMode !== 2 && mfgSteps.some((step) => step !== 0))) {
    throw new RangeError("states contains a reserved or inconsistent encoding");
  }
  return Object.freeze({
    role: roleNames[role],
    bootIntent: intentNames[bootIntent],
    installState: installNames[installState],
    upgradePhase: upgradeNames[upgradePhase],
    flagsKnown,
    appConfirmed: (value & (1n << 12n)) !== 0n,
    manualHold: (value & (1n << 13n)) !== 0n,
    installedValid: (value & (1n << 14n)) !== 0n,
    stagedValid: (value & (1n << 15n)) !== 0n,
    mfgMode: mfgModeNames[mfgMode],
    mfgSteps,
  });
}

export function commandAvailable(commandAvailability, command) {
  if (!Number.isInteger(commandAvailability) || commandAvailability < 0 ||
      commandAvailability > H2LoaderCommands.ALL || !Number.isInteger(command) ||
      command <= 0 || (command & (command - 1)) !== 0 ||
      (command & H2LoaderCommands.ALL) === 0) {
    throw new TypeError("command availability and command bit are invalid");
  }
  return (commandAvailability & command) !== 0;
}
