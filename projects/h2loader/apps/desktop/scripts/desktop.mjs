import { spawn } from 'node:child_process';
import { resolve } from 'node:path';
import { application, prepareNative } from './prepare-native.mjs';

const [operation, ...args] = process.argv.slice(2);
if (!['dev', 'build'].includes(operation)) throw new Error('Usage: desktop.mjs dev|build [Tauri arguments]');
prepareNative();
const child = spawn(process.execPath, [resolve(application, 'frontend/node_modules/@tauri-apps/cli/tauri.js'), operation, ...args], {cwd: application, stdio: 'inherit'});
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => child.kill(signal));
child.on('error', error => { console.error(error); process.exitCode = 1; });
child.on('exit', code => { process.exitCode = code ?? 1; });
