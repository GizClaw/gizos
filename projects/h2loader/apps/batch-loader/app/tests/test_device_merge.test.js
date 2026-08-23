import { describe, expect, it } from "vitest";
import { mergeAuthorizedPorts } from "../src/device_merge";

const name = (index) => `H2 Device ${index}`;
const port = (vid = 0x303a, pid = 0x1001, label) => ({ label, usbVendorId: vid, usbProductId: pid });

describe("mergeAuthorizedPorts", () => {
  it("keeps identical-hardware rows one-to-one and never collapses labels", () => {
    const persisted = [
      { portId: "hist:1", name: "SZP", usbVendorId: 0x303a, usbProductId: 0x1001, selected: true },
      { portId: "hist:2", name: "AMOLED", usbVendorId: 0x303a, usbProductId: 0x1001, selected: true },
    ];
    const a = port();
    const b = port();
    const merged = mergeAuthorizedPorts([a, b], persisted, name);
    expect(merged).toHaveLength(2);
    expect(merged.map((row) => row.name)).toEqual(["SZP", "AMOLED"]);
    expect(merged.map((row) => row.portId)).toEqual(["hist:1", "hist:2"]);
    expect(new Set(merged.map((row) => row.port)).size).toBe(2);
  });

  it("does not disturb an existing live row when a second port is authorized", () => {
    const a = port();
    const first = mergeAuthorizedPorts([a], [
      { portId: "hist:1", name: "SZP", usbVendorId: 0x303a, usbProductId: 0x1001 },
      { portId: "hist:2", name: "AMOLED", usbVendorId: 0x303a, usbProductId: 0x1001 },
    ], name);
    // mark the first row as freshly scanned
    first[0] = { ...first[0], state: "success", board: "amoled", stale: false };
    const b = port();
    const second = mergeAuthorizedPorts([a, b], first, name);
    const rowA = second.find((row) => row.port === a);
    const rowB = second.find((row) => row.port === b);
    expect(rowA.name).toBe("SZP");
    expect(rowA.state).toBe("success");
    expect(rowA.board).toBe("amoled");
    expect(rowB.name).toBe("AMOLED");
    expect(rowB.port).not.toBe(rowA.port);
  });

  it("orders rows by stable device number ascending regardless of enumeration order", () => {
    const mk = () => ({ label: "", usbVendorId: 0x303a, usbProductId: 0x1001 });
    // Ports get their number on first sight, so enumerate them out of order.
    const first = mk();
    const second = mk();
    const third = mk();
    mergeAuthorizedPorts([first, second, third], [], name); // assigns 1,2,3
    const merged = mergeAuthorizedPorts([third, first, second], [], name);
    const numbers = merged.map((row) => Number(/(\d+)$/.exec(row.portId)[1]));
    expect(numbers).toEqual([...numbers].sort((a, b) => a - b));
  });

  it("keeps a disconnected device in numeric position instead of floating", () => {
    const mk = () => ({ label: "", usbVendorId: 0x303a, usbProductId: 0x1001 });
    const one = mk();
    const two = mk();
    const three = mk();
    const bound = mergeAuthorizedPorts([one, two, three], [], name);
    const ids = bound.map((row) => row.portId);
    // device two disconnects: only one and three are still enumerated
    const after = mergeAuthorizedPorts([one, three], bound, name);
    expect(after.map((row) => row.portId)).toEqual(ids);
    expect(after[1].state).toBe("offline");
  });

  it("marks a previously authorized port offline when it disappears", () => {
    const a = port();
    const bound = mergeAuthorizedPorts([a], [{ portId: "hist:1", name: "SZP", usbVendorId: 0x303a, usbProductId: 0x1001 }], name);
    const gone = mergeAuthorizedPorts([], bound, name);
    expect(gone).toHaveLength(1);
    expect(gone[0].state).toBe("offline");
    expect(gone[0].port).toBeUndefined();
  });

  it("names label-less identical-hardware ports distinctly", () => {
    const a = { label: "", usbVendorId: 0x303a, usbProductId: 0x1001 };
    const b = { label: "", usbVendorId: 0x303a, usbProductId: 0x1001 };
    const merged = mergeAuthorizedPorts([a, b], [], name);
    // Numbering comes from a session counter, so assert distinct and ordered.
    expect(new Set(merged.map((row) => row.name)).size).toBe(2);
    expect(new Set(merged.map((row) => row.portId)).size).toBe(2);
  });

  it("appends a brand-new port with no persisted match", () => {
    const merged = mergeAuthorizedPorts([port(0x1234, 0x5678, "cu.usbmodemZ")], [], name);
    expect(merged).toHaveLength(1);
    expect(merged[0].name).toMatch(/^H2 Device \d+$/);
    expect(merged[0].state).toBe("ready");
  });
});
