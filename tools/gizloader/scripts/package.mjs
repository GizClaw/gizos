import { execFileSync } from 'node:child_process';
import { copyFileSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { validateReleaseIdentity, writeArtifactMetadata } from './release-index.mjs';

const directory = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const root = resolve(directory, '../..');
const channelIndex = process.argv.indexOf('--channel');
const channel = channelIndex < 0 ? (process.env.GIZLOADER_CHANNEL ?? 'build') : process.argv[channelIndex + 1];
if (!['build', 'snapshot', 'release'].includes(channel)) throw new Error('Expected --channel build|snapshot|release');
const version = process.env.GIZLOADER_VERSION ?? JSON.parse(readFileSync(join(directory, 'package.json'))).version;
const commit = process.env.GIZLOADER_COMMIT ?? execFileSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim();
validateReleaseIdentity({ version, channel, commit });
const publicRelease = channel !== 'build';
const platform = `${process.platform}-${process.arch}`;
if (!['darwin-arm64', 'win32-x64'].includes(platform)) throw new Error(`No installer for ${platform}`);
const mac = process.platform === 'darwin';
const identity = process.env.APPLE_SIGNING_IDENTITY || undefined;
if (mac && publicRelease && (!identity?.startsWith('Developer ID Application:') ||
    !process.env.APPLE_API_KEY || !process.env.APPLE_API_KEY_ID || !process.env.APPLE_API_ISSUER)) {
  throw new Error('Public macOS releases require a Developer ID identity and Apple notarization credentials');
}
const execute = (command, args, options = {}) => execFileSync(command, args, { cwd: directory, stdio: 'inherit', ...options });
if (mac) execute('/usr/bin/plutil', ['-replace', 'GizloaderVersion', '-string', version, join(directory, 'src-tauri/Info.generated.plist')]);
const config = {
  version,
  bundle: mac ? { macOS: { infoPlist: 'Info.generated.plist', signingIdentity: identity ?? '-' } } : {},
};
const configPath = join(directory, 'src-tauri/gizloader.generated.conf.json');
writeFileSync(configPath, JSON.stringify(config, null, 2) + '\n');
// Use the locked local Tauri CLI, with Bun available to beforeBuildCommand.
execute(process.execPath, [join(directory, 'node_modules/@tauri-apps/cli/tauri.js'), 'build',
  '--config', configPath, '--bundles', mac ? 'app,dmg' : 'nsis'], {
  env: { ...process.env, ...(mac && process.env.APPLE_API_KEY ? {
    APPLE_API_KEY_PATH: process.env.APPLE_API_KEY,
    APPLE_API_KEY: process.env.APPLE_API_KEY_ID,
  } : {}) },
});
const cargoTarget = process.env.CARGO_TARGET_DIR ? resolve(directory, process.env.CARGO_TARGET_DIR) : join(directory, 'src-tauri/target');
const bundle = join(cargoTarget, 'release/bundle');
const output = join(directory, 'release');
mkdirSync(output, { recursive: true });
let signing = { status: 'unsigned' };
let source;
if (mac) {
  const app = join(bundle, 'macos/Gizloader.app');
  execute('codesign', ['--verify', '--deep', '--strict', '--verbose=2', app]);
  const candidates = readdirSync(join(bundle, 'dmg')).filter(name => name.includes(`_${version}_`) && name.endsWith('.dmg'));
  if (candidates.length !== 1) throw new Error('Expected exactly one DMG');
  source = join(bundle, 'dmg', candidates[0]);
  if (identity) {
    // A DMG signature is separate from the signed/notarized app inside it.
    execute('codesign', ['--force', '--timestamp', '--sign', identity, source]);
    execute('codesign', ['--verify', '--strict', source]);
    signing = { status: 'signed', identity, notarized: false };
  } else signing = { status: 'ad-hoc', notarized: false };
  if (publicRelease) {
    execute('xcrun', ['stapler', 'validate', app]);
    execute('spctl', ['--assess', '--type', 'execute', '--verbose=2', app]);
    execute('xcrun', ['notarytool', 'submit', source, '--key', process.env.APPLE_API_KEY,
      '--key-id', process.env.APPLE_API_KEY_ID, '--issuer', process.env.APPLE_API_ISSUER, '--wait', '--timeout', '20m']);
    execute('xcrun', ['stapler', 'staple', source]);
    execute('xcrun', ['stapler', 'validate', source]);
    signing.notarized = true;
  }
} else {
  const candidates = readdirSync(join(bundle, 'nsis')).filter(name => name.includes(`_${version}_`) && name.endsWith('.exe'));
  if (candidates.length !== 1) throw new Error('Expected exactly one NSIS installer');
  source = join(bundle, 'nsis', candidates[0]);
}
const file = mac ? `gizloader-${version}-macos-arm64.dmg` : `gizloader-${version}-windows-x64-setup.exe`;
copyFileSync(source, join(output, file));
await writeArtifactMetadata({ directory: output, version, channel, commit, platform, app: mac ? 'Gizloader.app' : undefined, signing });
console.log(`Packaged ${file}`);
