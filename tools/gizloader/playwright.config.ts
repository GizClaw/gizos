import { defineConfig } from '@playwright/test'

export default defineConfig({
  testDir: './e2e',
  outputDir: './release/test-results',
  use: { baseURL: 'http://127.0.0.1:4176', viewport: { width: 1440, height: 900 } },
  reporter: 'list',
  timeout: 30000,
})
