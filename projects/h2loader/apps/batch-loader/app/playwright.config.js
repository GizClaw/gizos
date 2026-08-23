import path from "node:path";

const runfilesRoot = process.env.RUNFILES_DIR || process.env.TEST_SRCDIR;
const chromiumExecutable = runfilesRoot && process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE
  ? path.resolve(
    runfilesRoot,
    "_main",
    process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE,
  )
  : undefined;
const testServer = process.env.TEST_SERVER_ROOTPATH && runfilesRoot
  ? `${runfilesRoot}/_main/${process.env.TEST_SERVER_ROOTPATH}`
  : undefined;
const testArchive = process.env.TEST_ARCHIVE_ROOTPATH && runfilesRoot
  ? `${runfilesRoot}/_main/${process.env.TEST_ARCHIVE_ROOTPATH}`
  : undefined;

export default {
  testDir: "./tests",
  testMatch: "**/*.spec.cjs",
  timeout: 30_000,
  use: {
    baseURL: process.env.H2LOADER_WEB_BASE_URL || "http://127.0.0.1:4173",
    launchOptions: chromiumExecutable ? { executablePath: chromiumExecutable } : undefined,
    trace: "retain-on-failure",
  },
  reporter: "line",
  webServer: testServer && testArchive ? {
    command: `${testServer} --archive ${testArchive} --port 4173`,
    url: "http://127.0.0.1:4173",
    reuseExistingServer: false,
  } : undefined,
};
