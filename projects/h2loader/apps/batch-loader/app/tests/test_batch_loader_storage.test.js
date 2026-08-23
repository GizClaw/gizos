import { beforeEach, describe, expect, it } from "vitest";
import { loadDeviceMetadata, saveDeviceMetadata } from "../src/batch_loader_storage";

describe("device metadata storage", () => {
  beforeEach(() => localStorage.clear());

  it("persists stable device metadata without browser port objects", () => {
    saveDeviceMetadata([{ portId: "webserial:7", name: "Station 7", serial: "H2-007", board: "amoled", target: "esp32s3", role: "app", version: "1.0", usbVendorId: 0x303a, usbProductId: 0x1001, selected: true, lastResult: "install", port: { native: true }, file: new File(["x"], "firmware"), state: "ready" }]);
    expect(loadDeviceMetadata()).toEqual([{ portId: "webserial:7", name: "Station 7", serial: "H2-007", board: "amoled", target: "esp32s3", role: "app", version: "1.0", usbVendorId: 0x303a, usbProductId: 0x1001, selected: true, stale: true, state: "stale", lastResult: "install" }]);
  });

  it("recovers from invalid local storage", () => {
    localStorage.setItem("h2loader.batch-loader.devices.v1", "not-json");
    expect(loadDeviceMetadata()).toEqual([]);
  });
});
