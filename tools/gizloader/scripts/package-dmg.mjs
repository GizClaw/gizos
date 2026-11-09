import { execFileSync, spawnSync } from 'node:child_process';
import { copyFileSync, mkdirSync, mkdtempSync, renameSync, rmSync, symlinkSync } from 'node:fs';
import { join } from 'node:path';

const commandOptions = { timeout: 10 * 60 * 1000, killSignal: 'SIGKILL' };
const busyCreateLine = /^hdiutil: create failed - Resource busy\r?$/m;

function pause(milliseconds) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, milliseconds);
}

function createImage(contents, temporary, spawn, wait) {
  for (let attempt = 0; attempt < 3; attempt++) {
    const image = join(temporary, `Gizloader-${attempt + 1}.dmg`);
    const result = spawn('/usr/bin/hdiutil', ['create', '-srcfolder', contents, '-volname', 'Gizloader',
      '-fs', 'HFS+', '-format', 'UDZO', '-imagekey', 'zlib-level=9', image], {
      ...commandOptions, stdio: ['ignore', 'inherit', 'pipe'], maxBuffer: 1024 * 1024,
    });
    if (result.stderr?.length) process.stderr.write(result.stderr);
    if (result.error) throw result.error;
    if (result.status === 0 && result.signal === null) return image;
    // Only the exact transient create failure is retried; timeouts and signals
    // propagate immediately, and each attempt owns a fresh candidate filename.
    if (result.status !== 1 || result.signal !== null ||
        !busyCreateLine.test(result.stderr?.toString() ?? '') || attempt === 2) {
      throw Object.assign(new Error('hdiutil create failed'), { status: result.status, signal: result.signal });
    }
    wait((attempt + 1) * 1000);
  }
}

// Stage complete volume contents without requesting a writable image mount.
export function packageDmg({ bundle, version, arch, icon, execute = execFileSync, spawn = spawnSync, wait = pause }) {
  if (!['arm64', 'x64'].includes(arch)) throw new Error(`Unsupported DMG architecture: ${arch}`);
  const directory = join(bundle, 'dmg');
  mkdirSync(directory, { recursive: true });
  const temporary = mkdtempSync(join(directory, '.gizloader-dmg-'));
  const contents = join(temporary, 'contents');
  // Preserve Tauri's existing bundle filename so repeated builds replace it.
  const bundleArch = arch === 'arm64' ? 'aarch64' : arch;
  const destination = join(directory, `Gizloader_${version}_${bundleArch}.dmg`);
  const run = (command, args) => execute(command, args, { ...commandOptions, stdio: 'inherit' });
  try {
    mkdirSync(contents);
    // ditto preserves bundle symlinks, modes, extended attributes and signatures.
    run('/usr/bin/ditto', [join(bundle, 'macos/Gizloader.app'), join(contents, 'Gizloader.app')]);
    symlinkSync('/Applications', join(contents, 'Applications'));
    copyFileSync(icon, join(contents, '.VolumeIcon.icns'));
    run('/usr/bin/SetFile', ['-c', 'icnC', join(contents, '.VolumeIcon.icns')]);
    run('/usr/bin/SetFile', ['-a', 'C', contents]);
    const image = createImage(contents, temporary, spawn, wait);
    // Verification failure never retries creation or replaces a valid artifact.
    run('/usr/bin/hdiutil', ['verify', image]);
    renameSync(image, destination);
    return destination;
  } finally {
    // Only the unique staging directory created by this invocation is removed.
    rmSync(temporary, { recursive: true, force: true });
  }
}
