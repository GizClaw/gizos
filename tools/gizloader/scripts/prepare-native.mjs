import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { chmodSync, copyFileSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const directory = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const root = resolve(directory, '../..');
const targets = {
  'darwin-arm64': { triple: 'aarch64-apple-darwin', config: 'macos_arm64' },
  'win32-x64': { triple: 'x86_64-pc-windows-msvc', config: 'windows_x86_64' },
  'linux-x64': { triple: 'x86_64-unknown-linux-gnu', config: 'linux_x86_64' },
  'linux-arm64': { triple: 'aarch64-unknown-linux-gnu', config: 'linux_arm64' },
};
const host = `${process.platform}-${process.arch}`;
const target = targets[host];
if (!target) throw new Error(`Unsupported native build host: ${host}`);
const description = process.env.GIZLOADER_BLUETOOTH_DESCRIPTION ?? 'Gizloader uses Bluetooth to discover and communicate with GizOS devices.';
if (!description.trim() || description.length > 1024 || /[\u0000-\u0008\u000b\u000c\u000e-\u001f]/.test(description)) {
  throw new Error('GIZLOADER_BLUETOOTH_DESCRIPTION must contain 1–1024 characters of permission text');
}
const xml = value => value.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;');
const plist = `<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict>\n<key>NSBluetoothAlwaysUsageDescription</key><string>${xml(description)}</string>\n<key>NSBluetoothPeripheralUsageDescription</key><string>${xml(description)}</string>\n</dict></plist>\n`;
writeFileSync(join(directory, 'src-tauri/Info.generated.plist'), plist);
writeFileSync(join(directory, 'src-tauri/gizloader.generated.conf.json'), JSON.stringify({ bundle: { macOS: { infoPlist: 'Info.generated.plist' } } }, null, 2) + '\n');
const bazel = process.env.BAZEL_BIN ?? 'bazel';
const options = [`--config=${target.config}`, '-c', 'opt', `--//tools/gizloader/native:bluetooth_description=${description}`];
if (process.platform === 'win32') options.push('--copt=/wd5287', '--features=static_link_msvcrt', '--lockfile_mode=off');
const labels = ['//tools/gizloader/native:gizloader-cli', '//tools/gizloader/native:gizloader-session'];
execFileSync(bazel, ['build', ...options, ...labels], { cwd: root, stdio: 'inherit' });
const output = execFileSync(bazel, ['info', ...options, 'bazel-bin'], { cwd: root, encoding: 'utf8' }).trim();
const binaries = join(directory, 'src-tauri/binaries');
mkdirSync(binaries, { recursive: true });
const extension = process.platform === 'win32' ? '.exe' : '';
const hashes = {};
for (const name of ['gizloader-cli', 'gizloader-session']) {
  const source = join(output, 'tools/gizloader/native', name + extension);
  const destination = join(binaries, `${name}-${target.triple}${extension}`);
  copyFileSync(source, destination);
  // Bazel outputs are read-only; bundling must clear xattrs and sign its copy.
  chmodSync(destination, 0o755);
  hashes[name] = createHash('sha256').update(readFileSync(destination)).digest('hex');
}
const commit = execFileSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim();
writeFileSync(join(binaries, 'manifest.json'), JSON.stringify({
  schema: 1, commit, target: target.triple, bluetoothDescription: description,
  sha256: hashes['gizloader-cli'], session: { protocol: 1, sha256: hashes['gizloader-session'] },
}, null, 2) + '\n');
// Exercise the exact staged binary through pipes before bundling/signing.
const help = execFileSync(join(binaries, `gizloader-cli-${target.triple}${extension}`), ['--help'], { encoding: 'utf8' });
if (!help.includes('scan') || !help.includes('package')) throw new Error('Native command smoke check failed');
console.log(`Prepared Gizloader native sidecars for ${target.triple} (${commit})`);
