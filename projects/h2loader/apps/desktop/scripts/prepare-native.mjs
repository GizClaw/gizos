import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { chmodSync, copyFileSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const application = resolve(dirname(fileURLToPath(import.meta.url)), '..');
export const repository = resolve(application, '../../../..');
export function run(command, args, cwd = repository) {
  const result = spawnSync(command, args, { cwd, encoding: 'utf8', stdio: ['ignore', 'pipe', 'inherit'] });
  if (result.error || result.status !== 0) throw result.error || new Error(`${command} failed (${result.status})`);
  return result.stdout.trim();
}
export function sessionSourceDigest(root = repository) {
  const hash = createHash('sha256');
  for (const name of ['BUILD.bazel', 'session.c', 'session_test.c']) {
    hash.update(name).update('\0').update(readFileSync(resolve(root, 'projects/h2loader/targets/cc_binary/desktop_session', name)));
  }
  return hash.digest('hex');
}
export function prepareNative() {
  const triple = process.env.H2LOADER_DESKTOP_TARGET || run('rustc', ['--print', 'host-tuple']);
  const config = {
    'aarch64-apple-darwin': 'macos_arm64',
    'x86_64-unknown-linux-gnu': 'linux_x86_64',
    'aarch64-unknown-linux-gnu': 'linux_arm64',
    'x86_64-pc-windows-msvc': 'windows_x86_64',
  }[triple];
  if (!config) throw new Error(`Unsupported native desktop target: ${triple}`);
  const options = triple.includes('windows') ? ['--features=static_link_msvcrt', '--per_file_copt=libs/pal/providers/(wolfssl|windows/pal_core)/.*\\.c@/wd5287'] : [];
  const bazel = process.env.H2LOADER_DESKTOP_BAZEL || 'bazel';
  const directory = resolve(application, 'src-tauri/binaries');
  mkdirSync(directory, { recursive: true });
  mkdirSync(resolve(application, 'src-tauri/licenses'), { recursive: true });
  const digest = file => createHash('sha256').update(readFileSync(file)).digest('hex');
  const binaries = {};
  for (const [name, target] of [
    ['h2loader', '//projects/h2loader/targets/cc_binary/cli:h2loader'],
    ['h2loader-desktop-session', '//projects/h2loader/targets/cc_binary/desktop_session:h2loader-desktop-session'],
  ]) {
    run(bazel, ['build', `--config=${config}`, ...options, target]);
    const files = run(bazel, ['cquery', `--config=${config}`, ...options, '--output=files', target]).split(/\r?\n/);
    const file = files.find(file => file.endsWith(`/${name}`) || file.endsWith(`/${name}.exe`));
    if (!file) throw new Error(`Bazel did not declare ${name}`);
    const destination = resolve(directory, `${name}-${triple}${triple.includes('windows') ? '.exe' : ''}`);
    copyFileSync(resolve(repository, file), destination);
    chmodSync(destination, 0o755);
    binaries[name] = digest(destination);
  }
  run(bazel, ['test', `--config=${config}`, ...options, '//projects/h2loader/targets/cc_binary/desktop_session:desktop_session_test']);
  const manifest = {
    repository: 'https://github.com/GizClaw/gizos.git',
    commit: run('git', ['rev-parse', 'HEAD']), targetTriple: triple,
    target: '//projects/h2loader/targets/cc_binary/cli:h2loader',
    sha256: binaries.h2loader,
    session: {protocol: 1, sourceSha256: sessionSourceDigest(), sha256: binaries['h2loader-desktop-session']},
  };
  writeFileSync(resolve(directory, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
  copyFileSync(resolve(repository, 'LICENSE'), resolve(application, 'src-tauri/licenses/h2loader.txt'));
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) prepareNative();
