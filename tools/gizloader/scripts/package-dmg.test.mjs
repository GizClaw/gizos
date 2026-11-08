import assert from 'node:assert/strict';
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, readlinkSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { packageDmg } from './package-dmg.mjs';
import { execFileSync } from 'node:child_process';

function fixture(suffix = 'x64') {
  const root = mkdtempSync(join(tmpdir(), 'gizloader-package-dmg-test-'));
  const bundle = join(root, 'bundle');
  mkdirSync(join(bundle, 'macos/Gizloader.app/Contents/MacOS'), { recursive: true });
  writeFileSync(join(bundle, 'macos/Gizloader.app/Contents/MacOS/gizloader'), 'binary');
  mkdirSync(join(bundle, 'dmg'));
  writeFileSync(join(bundle, `dmg/Gizloader_0.1.0_${suffix}.dmg`), 'stale');
  const icon = join(root, 'icon.icns');
  writeFileSync(icon, 'icon');
  return { root, bundle, icon };
}

test('stages one offscreen volume and removes only its own staging directory', () => {
  const { root, bundle, icon } = fixture();
  try {
    const calls = [];
    let staging;
    const execute = (command, args, options) => {
      assert.equal(options.timeout, 600000);
      assert.equal(options.killSignal, 'SIGKILL');
      calls.push([command, args]);
      if (command === '/usr/bin/ditto') cpSync(args[0], args[1], { recursive: true });
      if (command === '/usr/bin/hdiutil' && args[0] === 'create') {
        const contents = args[args.indexOf('-srcfolder') + 1];
        staging = join(contents, '..');
        assert.equal(readFileSync(join(contents, 'Gizloader.app/Contents/MacOS/gizloader'), 'utf8'), 'binary');
        assert.equal(readlinkSync(join(contents, 'Applications')), '/Applications');
        assert.equal(readFileSync(join(contents, '.VolumeIcon.icns'), 'utf8'), 'icon');
        writeFileSync(args.at(-1), 'dmg');
      }
      if (command === '/usr/bin/hdiutil' && args[0] === 'verify') {
        assert.equal(readFileSync(args[1], 'utf8'), 'dmg');
      }
    };
    const destination = packageDmg({ bundle, version: '0.1.0', arch: 'x64', icon, execute });
    assert.equal(destination, join(bundle, 'dmg/Gizloader_0.1.0_x64.dmg'));
    assert.equal(readFileSync(destination, 'utf8'), 'dmg');
    assert.deepEqual(calls.map(([command, args]) => [command, args[0]]), [
      ['/usr/bin/ditto', join(bundle, 'macos/Gizloader.app')],
      ['/usr/bin/SetFile', '-c'],
      ['/usr/bin/SetFile', '-a'],
      ['/usr/bin/hdiutil', 'create'],
      ['/usr/bin/hdiutil', 'verify'],
    ]);
    assert.deepEqual(calls[3][1].slice(-3), ['-imagekey', 'zlib-level=9', join(staging, 'Gizloader.dmg')]);
    assert.equal(existsSync(staging), false);
    assert.equal(existsSync(join(bundle, 'dmg/Gizloader_0.1.0_x64.dmg')), true);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

for (const failure of ['ditto', 'SetFile', 'create', 'verify']) {
  test(`removes staging and leaves the previous DMG when ${failure} fails`, () => {
    const { root, bundle, icon } = fixture();
    try {
      let staging;
      const execute = (command, args) => {
        const stage = command === '/usr/bin/ditto' ? 'ditto' : command === '/usr/bin/SetFile' ? 'SetFile' :
          command === '/usr/bin/hdiutil' ? args[0] : undefined;
        if (stage === 'ditto' || stage === 'SetFile') staging ??= join(args.at(-1), '..');
        if (stage === 'create') {
          staging ??= join(args[args.indexOf('-srcfolder') + 1], '..');
          writeFileSync(args.at(-1), 'partial');
        }
        if (stage === failure) throw new Error(`${failure} failed`);
      };
      assert.throws(() => packageDmg({ bundle, version: '0.1.0', arch: 'x64', icon, execute }), new RegExp(`${failure} failed`));
      assert.deepEqual(readdirSync(join(bundle, 'dmg')).filter(name => name.startsWith('.')),
        [], 'staging directory was not removed');
      assert.equal(existsSync(staging), false);
      assert.equal(readFileSync(join(bundle, 'dmg/Gizloader_0.1.0_x64.dmg'), 'utf8'), 'stale');
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  });
}

test('creates a readable compressed image that stays unattached', { skip: process.platform !== 'darwin' }, () => {
  const { root, bundle, icon } = fixture();
  try {
    const mounted = () => execFileSync('/usr/bin/hdiutil', ['info'], { encoding: 'utf8' });
    const destination = packageDmg({ bundle, version: '0.1.0', arch: 'x64', icon });
    assert.equal(readFileSync(destination).subarray(-512, -508).toString('latin1'), 'koly');
    assert.equal(mounted().includes('gizloader-package-dmg-test-'), false);
    execFileSync('/usr/bin/hdiutil', ['verify', destination], { stdio: 'ignore' });
    assert.equal(existsSync(join(bundle, 'dmg/Gizloader_0.1.0_x64.dmg')), true);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('preserves the existing ARM64 bundle filename without adding a second installer', () => {
  const { root, bundle, icon } = fixture('aarch64');
  try {
    const execute = (command, args) => {
      if (command === '/usr/bin/ditto') cpSync(args[0], args[1], { recursive: true });
      if (command === '/usr/bin/hdiutil' && args[0] === 'create') writeFileSync(args.at(-1), 'dmg');
    };
    const destination = packageDmg({ bundle, version: '0.1.0', arch: 'arm64', icon, execute });
    assert.equal(destination, join(bundle, 'dmg/Gizloader_0.1.0_aarch64.dmg'));
    assert.deepEqual(readdirSync(join(bundle, 'dmg')), ['Gizloader_0.1.0_aarch64.dmg']);
    assert.equal(readFileSync(destination, 'utf8'), 'dmg');
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('rejects unknown architectures before creating staging', () => {
  const { root, bundle, icon } = fixture();
  try {
    assert.throws(() => packageDmg({ bundle, version: '0.1.0', arch: 'ppc', icon, execute() {} }), /Unsupported DMG architecture/);
    assert.deepEqual(readFileSync(join(bundle, 'dmg/Gizloader_0.1.0_x64.dmg'), 'utf8'), 'stale');
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});
