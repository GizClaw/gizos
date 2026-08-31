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
  COREDUMP_STATUS: 1 << 8,
  COREDUMP_DUMP: 1 << 9,
  COREDUMP_ERASE: 1 << 10,
  STAGE_PAYLOAD: 1 << 11,
  STAGE_ABORT: 1 << 12,
  STAGE_URL: 1 << 13,
  WIFI_SCAN: 1 << 16,
  WIFI_CONNECT: 1 << 17,
  WIFI_DISCONNECT: 1 << 18,
  REBOOT_UPGRADE: 1 << 19,
  ALL: 0x000f3f3f,
});

export function commandAvailable(commandAvailability, command) {
  if (!Number.isInteger(commandAvailability) || commandAvailability < 0 ||
      commandAvailability > H2LoaderCommands.ALL || !Number.isInteger(command) ||
      command <= 0 || (command & (command - 1)) !== 0 ||
      (command & H2LoaderCommands.ALL) === 0) {
    throw new TypeError("command availability and command bit are invalid");
  }
  return (commandAvailability & command) !== 0;
}
