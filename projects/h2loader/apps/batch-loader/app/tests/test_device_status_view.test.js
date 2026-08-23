import { describe, expect, it } from "vitest";
import { appAvailability, capabilityList, shortChecksum, statusRows } from "../src/device_status_view";

describe("device status view", () => {
  it("reports what the device can boot", () => {
    expect(appAvailability({ installedValid: 1 })).toBe("installed");
    expect(appAvailability({ installedValid: 0, stagedValid: 1 })).toBe("staged");
    expect(appAvailability({ installedValid: 0, stagedValid: 0 })).toBe("none");
    expect(appAvailability({ role: "app" })).toBe("running");
    expect(appAvailability({})).toBe("unknown");
  });

  it("expands capability bits into named flags", () => {
    const list = capabilityList({ capabilities: 2 | 8 });
    expect(list.find((item) => item.key === "stage").enabled).toBe(true);
    expect(list.find((item) => item.key === "reboot").enabled).toBe(true);
    expect(list.find((item) => item.key === "rollback").enabled).toBe(false);
    expect(capabilityList({}).every((item) => item.enabled === false)).toBe(true);
  });

  it("shortens checksums and leaves short values alone", () => {
    expect(shortChecksum("a".repeat(64))).toBe("aaaaaaaa…aaaa");
    expect(shortChecksum("")).toBe("");
  });

  it("nulls out absent fields instead of inventing values", () => {
    const rows = statusRows({ role: "h2loader", board: "amoled", stagedBytes: 0 });
    expect(rows.find((r) => r.key === "role").value).toBe("h2loader");
    expect(rows.find((r) => r.key === "board").value).toBe("amoled");
    expect(rows.find((r) => r.key === "stagedBytes").value).toBeNull();
    expect(rows.find((r) => r.key === "version").value).toBeNull();
  });
});
