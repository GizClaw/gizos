const crypto = require("node:crypto");
const zlib = require("node:zlib");
const { expect, test } = require("@playwright/test");

function tarFile(path, data) {
  const padded = Math.ceil(data.length / 512) * 512;
  const entry = Buffer.alloc(512 + padded);
  entry.write(path, 0, "utf8");
  entry.write(data.length.toString(8).padStart(11, "0"), 124, "ascii");
  entry.fill(0x20, 148, 156);
  entry[156] = 0x30;
  let checksum = 0;
  for (let index = 0; index < 512; ++index) checksum += entry[index];
  entry.write(checksum.toString(8).padStart(6, "0"), 148, "ascii");
  entry[154] = 0;
  entry[155] = 0x20;
  data.copy(entry, 512);
  return entry;
}

function packageBytes(role = "app") {
  const image = Buffer.from("batch-loader-image");
  const imageSha = crypto.createHash("sha256").update(image).digest("hex");
  const dataSha = crypto.createHash("sha256").digest("hex");
  const manifest = Buffer.from(
    `format=1\nrole=${role}\nboard=devkit\ntarget=esp32s3\n` +
    `version=v1.2.3\nimage_size=${image.length}\nimage_sha256=${imageSha}\n`,
  );
  const tar = Buffer.concat([
    tarFile("manifest", manifest),
    tarFile("checksum", Buffer.from(`${dataSha}\n`)),
    tarFile("app/esp/app.bin", image),
    Buffer.alloc(1024),
  ]);
  return zlib.deflateSync(tar, { level: 1 });
}

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => {
    const parameters = new URLSearchParams(location.search);
    const unsupported = parameters.get("unsupported") === "1";
    const count = unsupported ? 0 : Number(parameters.get("ports") || 1);
    const hangOpen = parameters.get("hang") === "1";
    globalThis.__batchTest = { active: 0, peak: 0, openCount: 0, requestCount: 0 };
    const createPort = (index) => { const port = {
      connected: true,
      getInfo: () => ({ usbVendorId: 0x303a, usbProductId: 0x1000 + index }),
      open: () => {
        globalThis.__batchTest.openCount += 1;
        globalThis.__batchTest.active += 1;
        globalThis.__batchTest.peak = Math.max(
          globalThis.__batchTest.peak,
          globalThis.__batchTest.active,
        );
        // Keep the port busy so a cancellation assertion is not racing the
        // injected failure.
        if (hangOpen) return new Promise(() => {});
        return new Promise((_, reject) => setTimeout(() => {
          globalThis.__batchTest.active -= 1;
          reject(new DOMException("Injected serial disconnect", "NetworkError"));
        }, 120));
      },
      close: async () => {},
      forget: async () => {
        globalThis.__batchTest.forgetCount = (globalThis.__batchTest.forgetCount || 0) + 1;
        const at = ports.indexOf(port);
        if (at >= 0) ports.splice(at, 1);
      },
    }; return port; };
    const ports = Array.from({ length: count }, (_, index) => createPort(index + 1));
    Object.defineProperty(globalThis, "isSecureContext", {
      configurable: true,
      value: true,
    });
    Object.defineProperty(navigator, "serial", {
      configurable: true,
      value: unsupported ? undefined : {
        getPorts: async () => ports,
        requestPort: async () => {
          globalThis.__batchTest.requestCount += 1;
          globalThis.__batchTest.userActivation = navigator.userActivation?.isActive === true;
          const port = createPort(ports.length + 1);
          ports.push(port);
          return port;
        },
        addEventListener: () => {},
        removeEventListener: () => {},
      },
    });
  });
  await page.goto("/");
});

test("restores authorized devices through the real JS/WASM SDK", async ({ page }) => {
  await expect(page.getByTestId("device-row")).toHaveCount(1);
  await expect(page.getByTestId("device-row")).toContainText("Device 1");
});

test("authorizes, appends and auto-scans a port from direct user activation", async ({ page }) => {
  await page.getByRole("button", { name: "Add device" }).click();
  await expect(page.getByTestId("device-row")).toHaveCount(2);
  expect(await page.evaluate(() => globalThis.__batchTest.requestCount)).toBe(1);
  expect(await page.evaluate(() => globalThis.__batchTest.userActivation)).toBe(true);
  await expect(page.getByTestId("device-row").nth(1).getByText("Failed", { exact: true })).toBeVisible();
  expect(await page.evaluate(() => globalThis.__batchTest.openCount)).toBe(1);
  await expect(page.getByTestId("device-row").nth(0).getByText("Not scanned", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "Add device" }).click();
  await expect(page.getByTestId("device-row")).toHaveCount(3);
  await expect(page.getByTestId("device-row").nth(0).getByText("Not scanned", { exact: true })).toBeVisible();
  await expect(page.getByTestId("device-row").nth(1).getByText("Failed", { exact: true })).toBeVisible();
  expect(await page.evaluate(() => globalThis.__batchTest.openCount)).toBe(2);
});

test("forgets an authorized port through the real SDK", async ({ page }) => {
  await page.getByRole("button", { name: "Actions for Device 1" }).click();
  await page.getByRole("menuitem", { name: "Forget" }).click();
  await expect(page.getByTestId("device-row")).toHaveCount(0);
  expect(await page.evaluate(() => globalThis.__batchTest.forgetCount)).toBe(1);
  await page.reload();
  await expect(page.getByTestId("device-row")).toHaveCount(1);
});

test("shows a readable device status dialog", async ({ page }) => {
  await page.getByRole("button", { name: "Actions for Device 1" }).click();
  await page.getByRole("menuitem", { name: "View status" }).click();
  const dialog = page.getByRole("dialog");
  await expect(dialog).toContainText("Device status");
  await expect(dialog).toContainText("App availability");
  await expect(dialog).toContainText("Capabilities");
  // Unscanned device: availability is unknown and the scan hint is shown.
  await expect(dialog).toContainText("Unknown until the device is scanned");
});

test("switches the UI language and persists the choice", async ({ page }) => {
  await page.getByRole("button", { name: "中文" }).click();
  await expect(page.getByRole("button", { name: "添加设备" })).toBeVisible();
  await expect(page.locator("html")).toHaveAttribute("lang", "zh-CN");
  await page.reload();
  await expect(page.getByRole("button", { name: "添加设备" })).toBeVisible();
  await page.getByRole("button", { name: "EN" }).click();
  await expect(page.getByRole("button", { name: "Add device" })).toBeVisible();
});

test("inspects a real package in WASM and keeps it in the dialog", async ({ page }) => {
  await page.getByTestId("current-package").click();
  await page.getByTestId("firmware-file").setInputFiles({
    name: "devkit-app.update.tar.zlib",
    mimeType: "application/octet-stream",
    buffer: packageBytes("app"),
  });
  await expect(page.getByTestId("package-inspection")).toContainText("devkit · esp32s3");
  await expect(page.getByTestId("package-inspection")).toContainText("v1.2.3");
  await page.getByRole("button", { name: "Use APP" }).click();
  await expect(page.getByTestId("current-package")).toContainText("devkit-app.update.tar.zlib");
});

test("shows package role and format failures inside the dialog", async ({ page }) => {
  await page.getByTestId("current-package").click();
  await page.getByRole("button", { name: "Loader firmware" }).click();
  await page.getByTestId("firmware-file").setInputFiles({
    name: "wrong-role.update.tar.zlib",
    mimeType: "application/octet-stream",
    buffer: packageBytes("app"),
  });
  await expect(page.getByRole("alert")).toContainText("Expected h2loader package, received app");
  await page.getByTestId("firmware-file").setInputFiles({
    name: "invalid.update.tar.zlib",
    mimeType: "application/octet-stream",
    buffer: Buffer.from("not-a-package"),
  });
  await expect(page.getByRole("alert")).toContainText("Package rejected");
  await expect(page.getByRole("dialog")).toBeVisible();
});

test("scans through the real SDK and isolates serial failures", async ({ page }) => {
  await page.getByRole("button", { name: "Scan", exact: true }).click();
  await expect(page.getByText("Failed", { exact: true })).toBeVisible();
  expect(await page.evaluate(() => globalThis.__batchTest.openCount)).toBe(1);
});

test("restores persisted metadata as stale until a live scan", async ({ page }) => {
  // Wait for the initial refresh to settle: the App persists its device list on
  // every change, so seeding storage earlier would be overwritten.
  await expect(page.getByTestId("device-row")).toHaveCount(1);
  await page.evaluate(() => localStorage.setItem("h2loader.batch-loader.devices.v1", JSON.stringify([{
    portId: "historical:factory-01",
    name: "Station 01",
    role: "app",
    version: "1.8.0",
    selected: true,
  }])));
  await page.reload();
  await expect(page.getByTestId("device-row")).toHaveCount(1);
  await expect(page.getByText("Station 01")).toBeVisible();
  await expect(page.getByText("Stale", { exact: true })).toBeVisible();
});

test("scans every authorized device without the four-job write cap", async ({ page }) => {
  await page.goto("/?ports=5&hang=1");
  await expect(page.getByTestId("device-row")).toHaveCount(5);
  await page.getByRole("button", { name: "Scan", exact: true }).click();
  // Scans only read status, so all five ports open concurrently.
  await expect.poll(() => page.evaluate(() => globalThis.__batchTest.peak)).toBe(5);
  await page.getByRole("button", { name: "Cancel" }).click();
});

test("cancels active and queued real SDK jobs", async ({ page }) => {
  await page.goto("/?ports=5&hang=1");
  await expect(page.getByTestId("device-row")).toHaveCount(5);
  await page.getByRole("button", { name: "Scan", exact: true }).click();
  await expect(page.getByText("Scanning", { exact: true })).toHaveCount(5);
  await page.getByRole("button", { name: "Cancel" }).click();
  await expect(page.getByText("Cancelled", { exact: true })).toHaveCount(5);
});

test("synchronously fences SDK work when the page lifecycle ends", async ({ page }) => {
  await page.evaluate(() => globalThis.dispatchEvent(new PageTransitionEvent("pagehide")));
  await expect(page.getByRole("button", { name: "Add device" })).toBeDisabled();
  expect(await page.evaluate(() => globalThis.__batchTest.requestCount)).toBe(0);
});

test("shows unsupported state and disables authorization", async ({ page }) => {
  await page.goto("/?unsupported=1");
  await expect(page.getByText(/Web Serial requires/)).toBeVisible();
  await expect(page.getByRole("button", { name: "Add device" })).toBeDisabled();
});
