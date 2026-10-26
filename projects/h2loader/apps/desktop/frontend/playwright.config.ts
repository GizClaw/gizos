import {defineConfig, devices} from '@playwright/test'
export default defineConfig({testDir:'./e2e',workers:1,retries:0,timeout:30000,reporter:'line',use:{...devices['Desktop Chrome'],baseURL:'http://127.0.0.1:4177'},webServer:{command:'npm run dev -- --host 127.0.0.1 --port 4177 --strictPort',url:'http://127.0.0.1:4177/desktop/loader.html',reuseExistingServer:false}})
