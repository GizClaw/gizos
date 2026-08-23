import { describe, expect, it } from "vitest";
import { canRestart, canStage, canSwitchToApp, canSwitchToLoader } from "../src/device_actions";

const CAP = { STAGE: 2, REBOOT: 8, RESTART: 16, ROLLBACK: 32 };
const dev = (over) => ({ port: {}, ...over });

describe("device action eligibility", () => {
  it("unscanned rows (no role/caps) support nothing", () => {
    const d = dev({});
    expect(canSwitchToLoader(d)).toBe(false);
    expect(canSwitchToApp(d)).toBe(false);
    expect(canRestart(d)).toBe(false);
  });

  it("only a loader with STAGE can receive a staged package", () => {
    expect(canStage(dev({ role: "h2loader", capabilities: CAP.STAGE }))).toBe(true);
    expect(canStage(dev({ role: "h2loader", capabilities: CAP.REBOOT }))).toBe(false);
    expect(canStage(dev({ role: "app", capabilities: CAP.STAGE }))).toBe(false);
    expect(canStage(dev({}))).toBe(false);
  });

  it("app role: switch to loader + restart, not switch to app", () => {
    const d = dev({ role: "app", capabilities: CAP.ROLLBACK | CAP.RESTART });
    expect(canSwitchToLoader(d)).toBe(true);
    expect(canRestart(d)).toBe(true);
    expect(canSwitchToApp(d)).toBe(false);
  });

  it("loader role with an image: switch to app + restart, not switch to loader", () => {
    const d = dev({ role: "h2loader", capabilities: CAP.REBOOT, stagedValid: 1 });
    expect(canSwitchToApp(d)).toBe(true);
    expect(canRestart(d)).toBe(true);
    expect(canSwitchToLoader(d)).toBe(false);
  });

  it("loader role without any image cannot switch to app", () => {
    const d = dev({ role: "h2loader", capabilities: CAP.REBOOT, stagedValid: 0, installedValid: 0 });
    expect(canSwitchToApp(d)).toBe(false);
    expect(canRestart(d)).toBe(true);
  });

  it("offline row (no live port) supports nothing", () => {
    const d = { role: "app", capabilities: CAP.ROLLBACK | CAP.RESTART };
    expect(canSwitchToLoader(d)).toBe(false);
    expect(canRestart(d)).toBe(false);
  });
});
