import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'
import { defineConfig } from 'vite'
import path from 'node:path'

export default defineConfig({
  plugins: [react(), tailwindcss()],
  // Desktop uses the pinned SDK's command definitions and native I/O.
  resolve: { alias: [
    { find: /^@gizclaw\/h2loader$/, replacement: path.resolve(import.meta.dirname, '../../../libs/web/src/h2loader_protocol.js') },
    { find: '@', replacement: path.resolve(import.meta.dirname, './src') },
  ] },
  publicDir: false,
  clearScreen: false,
  server: { host: '127.0.0.1', port: 4177, strictPort: true },
  build: {
    outDir: 'dist',
    rolldownOptions: { input: path.resolve(import.meta.dirname, 'desktop/loader.html') },
  },
})
