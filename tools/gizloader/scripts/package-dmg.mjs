import { execFileSync } from 'node:child_process';
import { copyFileSync, mkdirSync, mkdtempSync, renameSync, rmSync, symlinkSync } from 'node:fs';
import { join } from 'node:path';

// Stage the complete volume before creating it. Unlike create-dmg's writable
// image workflow, this never attaches a device that Finder or indexers can hold.
export function packageDmg({ bundle, version, arch, icon, execute = execFileSync }) {
  if (!['arm64', 'x64'].includes(arch)) throw new Error(`Unsupported DMG architecture: ${arch}`);
  const directory = join(bundle, 'dmg');
  mkdirSync(directory, { recursive: true });
  const temporary = mkdtempSync(join(directory, '.gizloader-dmg-'));
  const contents = join(temporary, 'contents');
  const image = join(temporary, 'Gizloader.dmg');
  // Preserve Tauri's existing bundle filename so repeated builds replace it.
  const bundleArch = arch === 'arm64' ? 'aarch64' : arch;
  const destination = join(directory, `Gizloader_${version}_${bundleArch}.dmg`);
  const run = (command, args) => execute(command, args, {
    stdio: 'inherit', timeout: 10 * 60 * 1000, killSignal: 'SIGKILL',
  });
  try {
    mkdirSync(contents);
    // ditto preserves bundle symlinks, modes, extended attributes and signatures.
    run('/usr/bin/ditto', [join(bundle, 'macos/Gizloader.app'), join(contents, 'Gizloader.app')]);
    symlinkSync('/Applications', join(contents, 'Applications'));
    copyFileSync(icon, join(contents, '.VolumeIcon.icns'));
    run('/usr/bin/SetFile', ['-c', 'icnC', join(contents, '.VolumeIcon.icns')]);
    run('/usr/bin/SetFile', ['-a', 'C', contents]);
    run('/usr/bin/hdiutil', ['create', '-srcfolder', contents, '-volname', 'Gizloader',
      '-fs', 'HFS+', '-format', 'UDZO', '-imagekey', 'zlib-level=9', image]);
    run('/usr/bin/hdiutil', ['verify', image]);
    renameSync(image, destination);
    return destination;
  } finally {
    // Only the unique staging directory created by this invocation is removed.
    rmSync(temporary, { recursive: true, force: true });
  }
}
