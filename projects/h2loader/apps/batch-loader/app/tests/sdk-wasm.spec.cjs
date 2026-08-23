const { expect, test } = require("@playwright/test");

test("enumerates an authorized port through the real JS and WASM SDK", async ({ page }) => {
  await page.addInitScript(() => {
    Object.defineProperty(globalThis, "isSecureContext", { configurable: true, value: true });
    const port = {
      getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1001 }),
      readable: null,
      writable: null,
    };
    Object.defineProperty(navigator, "serial", { configurable: true, value: {
      getPorts: async () => [port],
      requestPort: async () => port,
      addEventListener: () => {},
      removeEventListener: () => {},
    } });
  });
  await page.goto("/");
  const result = await page.evaluate(async () => {
    const { createH2Loader } = await import("/sdk/h2loader.js");
    const loader = await createH2Loader();
    try {
      const ports = await loader.getAuthorizedPorts();
      return {
        supported: loader.supported(),
        ports: ports.map((port) => ({
          label: port.label,
          usbVendorId: port.usbVendorId,
          usbProductId: port.usbProductId,
        })),
      };
    } finally {
      await loader.close();
    }
  });
  expect(result).toEqual({
    supported: true,
    ports: [{ label: "Authorized serial device", usbVendorId: 0x303a, usbProductId: 0x1001 }],
  });
  await expect(page.getByTestId("device-row")).toHaveCount(1);
  await expect(page.getByTestId("device-row")).toContainText("Device 1");

  const shutdown = await page.evaluate(async () => {
    const { createH2Loader } = await import("/sdk/h2loader.js");
    const loader = await createH2Loader();
    const operation = loader.inspectPackage(new Blob([new Uint8Array(1024 * 1024)]));
    const results = await Promise.allSettled([operation, loader.close()]);
    return results.map(({ status, reason }) => ({ status, errorName: reason?.name }));
  });
  expect(shutdown).toEqual([
    { status: "rejected", errorName: "H2LoaderError" },
    { status: "fulfilled", errorName: undefined },
  ]);
});

test("enumerates more than 32 authorized ports through the real SDK", async ({ page }) => {
  await page.addInitScript(() => {
    Object.defineProperty(globalThis, "isSecureContext", { configurable: true, value: true });
    const ports = Array.from({ length: 40 }, (_, index) => ({
      getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1000 + index }),
      readable: null,
      writable: null,
    }));
    Object.defineProperty(navigator, "serial", { configurable: true, value: {
      getPorts: async () => ports,
      requestPort: async () => ports[0],
      addEventListener: () => {},
      removeEventListener: () => {},
    } });
  });
  await page.goto("/");
  const count = await page.evaluate(async () => {
    const { createH2Loader } = await import("/sdk/h2loader.js");
    const loader = await createH2Loader();
    try {
      return (await loader.getAuthorizedPorts()).length;
    } finally {
      await loader.close();
    }
  });
  expect(count).toBe(40);
});

test("stops chooser polling before concurrent close releases the client", async ({ page }) => {
  await page.addInitScript(() => {
    Object.defineProperty(globalThis, "isSecureContext", { configurable: true, value: true });
    let resolveChooser;
    Object.defineProperty(navigator, "serial", { configurable: true, value: {
      getPorts: async () => [],
      requestPort: () => new Promise((resolve) => { resolveChooser = resolve; }),
      addEventListener: () => {},
      removeEventListener: () => {},
    } });
    globalThis.__resolveChooser = () => resolveChooser?.({ getInfo: () => ({}) });
  });
  await page.goto("/");
  const results = await page.evaluate(async () => {
    const { createH2Loader } = await import("/sdk/h2loader.js");
    const loader = await createH2Loader({ pollIntervalMs: 1 });
    const chooser = loader.requestPort();
    await new Promise((resolve) => setTimeout(resolve, 0));
    const close = loader.close();
    globalThis.__resolveChooser();
    return Promise.allSettled([chooser, close]).then((settled) =>
      settled.map(({ status, reason }) => ({ status, name: reason?.name })));
  });
  expect(results).toEqual([
    { status: "rejected", name: "H2LoaderError" },
    { status: "fulfilled", name: undefined },
  ]);
});

test("rejects unsupported public creation options", async ({ page }) => {
  await page.goto("/");
  const error = await page.evaluate(async () => {
    const { createH2Loader } = await import("/sdk/h2loader.js");
    try {
      await createH2Loader({ pollIntervalMs: 0 });
      return null;
    } catch (reason) {
      return { name: reason.name, kind: reason.kind, operation: reason.operation };
    }
  });
  expect(error).toEqual({
    name: "H2LoaderError",
    kind: "invalid-argument",
    operation: "create",
  });
  const unknown = await page.evaluate(async () => {
    const { createH2Loader } = await import("/sdk/h2loader.js");
    try {
      await createH2Loader({ moduleFactory: () => {} });
      return null;
    } catch (reason) {
      return { name: reason.name, kind: reason.kind, operation: reason.operation };
    }
  });
  expect(unknown).toEqual({
    name: "H2LoaderError",
    kind: "invalid-argument",
    operation: "create",
  });
});
