import assert from 'node:assert/strict';
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, readlinkSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { packageDmg } from './package-dmg.mjs';
import { execFileSync } from 'node:child_process';

function createResult(overrides = {}) {
  return { status: 0, signal: null, stdout: null, stderr: null, ...overrides };
}

function fakeSpawn(execute) {
  return (command, args, options) => {
    execute(command, args, options);
    return createResult();
  };
}

function stageApp(command, args) {
  if (command === '/usr/bin/ditto') cpSync(args[0], args[1], { recursive: true });
}

const busyStderr = Buffer.from('hdiutil: create failed - Resource busy\n');

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
    const destination = packageDmg({ bundle, version: '0.1.0', arch: 'x64', icon, execute, spawn: fakeSpawn(execute) });
    assert.equal(destination, join(bundle, 'dmg/Gizloader_0.1.0_x64.dmg'));
    assert.equal(readFileSync(destination, 'utf8'), 'dmg');
    assert.deepEqual(calls.map(([command, args]) => [command, args[0]]), [
      ['/usr/bin/ditto', join(bundle, 'macos/Gizloader.app')],
      ['/usr/bin/SetFile', '-c'],
      ['/usr/bin/SetFile', '-a'],
      ['/usr/bin/hdiutil', 'create'],
      ['/usr/bin/hdiutil', 'verify'],
    ]);
    assert.deepEqual(calls[3][1].slice(-3), ['-imagekey', 'zlib-level=9', join(staging, 'Gizloader-1.dmg')]);
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
      assert.throws(() => packageDmg({ bundle, version: '0.1.0', arch: 'x64', icon, execute, spawn: fakeSpawn(execute) }), new RegExp(`${failure} failed`));
      assert.deepEqual(readdirSync(join(bundle, 'dmg')).filter(name => name.startsWith('.')),
        [], 'staging directory was not removed');
      assert.equal(existsSync(staging), false);
      assert.equal(readFileSync(join(bundle, 'dmg/Gizloader_0.1.0_x64.dmg'), 'utf8'), 'stale');
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  });
}

test('creates a verified UDZO image without leaving an attachment', { skip: process.platform !== 'darwin' }, () => {
  const { root, bundle, icon } = fixture();
  try {
    const options = { timeout: 60000, killSignal: 'SIGKILL' };
    const destination = packageDmg({ bundle, version: '0.1.0', arch: 'x64', icon });
    assert.equal(readFileSync(destination).subarray(-512, -508).toString('latin1'), 'koly');
    const info = execFileSync('/usr/bin/hdiutil', ['imageinfo', '-plist', destination], options);
    const metadata = JSON.parse(execFileSync('/usr/bin/plutil', ['-convert', 'json', '-o', '-', '--', '-'],
      { ...options, input: info, encoding: 'utf8' }));
    assert.equal(metadata.Format, 'UDZO');
    const mounted = execFileSync('/usr/bin/hdiutil', ['info'], { ...options, encoding: 'utf8' });
    assert.equal(mounted.includes('gizloader-package-dmg-test-'), false);
    execFileSync('/usr/bin/hdiutil', ['verify', destination], { ...options, stdio: 'ignore' });
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
    const destination = packageDmg({ bundle, version: '0.1.0', arch: 'arm64', icon, execute, spawn: fakeSpawn(execute) });
    assert.equal(destination, join(bundle, 'dmg/Gizloader_0.1.0_aarch64.dmg'));
    assert.deepEqual(readdirSync(join(bundle, 'dmg')), ['Gizloader_0.1.0_aarch64.dmg']);
    assert.equal(readFileSync(destination, 'utf8'), 'dmg');
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

for (const succeedsOn of [2, 3]) {
  test(`retries the exact busy failure and succeeds on attempt ${succeedsOn}`, t => {
    const { root, bundle, icon } = fixture();
    const stderr = [];
    t.mock.method(process.stderr, 'write', chunk => { stderr.push(chunk.toString()); return true; });
    try {
      const candidates = [], delays = [];
      let verifies = 0;
      const execute = (command, args) => {
        stageApp(command, args);
        if (command === '/usr/bin/hdiutil') {
          assert.equal(args[0], 'verify');
          verifies++;
          assert.equal(readFileSync(args[1], 'utf8'), 'complete');
          assert.equal(readFileSync(join(bundle, 'dmg/Gizloader_0.1.0_x64.dmg'), 'utf8'), 'stale');
        }
      };
      const spawn = (command, args, options) => {
        assert.equal(command, '/usr/bin/hdiutil');
        assert.equal(args[0], 'create');
        assert.equal(options.timeout, 600000);
        assert.equal(options.killSignal, 'SIGKILL');
        assert.equal(options.maxBuffer, 1024 * 1024);
        assert.deepEqual(options.stdio, ['ignore', 'inherit', 'pipe']);
        const image = args.at(-1);
        candidates.push(image);
        const success = candidates.length === succeedsOn;
        writeFileSync(image, success ? 'complete' : 'partial');
        return createResult({ status: success ? 0 : 1,
          stderr: success ? Buffer.from('creation warning\n') : busyStderr });
      };
      const destination = packageDmg({ bundle, version: '0.1.0', arch: 'x64', icon,
        execute, spawn, wait: ms => delays.push(ms) });
      t.mock.restoreAll();
      assert.equal(readFileSync(destination, 'utf8'), 'complete');
      assert.equal(candidates.length, succeedsOn);
      assert.equal(new Set(candidates).size, succeedsOn);
      assert.deepEqual(delays, [1000, 2000].slice(0, succeedsOn - 1));
      assert.equal(verifies, 1);
      assert.equal(stderr.join(''), busyStderr.toString().repeat(succeedsOn - 1) + 'creation warning\n');
      assert.ok(candidates.every(image => !existsSync(image)));
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  });
}

test('exhausts three busy attempts without replacing the previous installer', t => {
  const { root, bundle, icon } = fixture();
  t.mock.method(process.stderr, 'write', () => true);
  try {
    const unrelated = join(bundle, 'dmg/.unrelated');
    mkdirSync(unrelated);
    writeFileSync(join(unrelated, 'keep'), 'owned elsewhere');
    const candidates = [], delays = [];
    const spawn = (command, args) => {
      const image = args.at(-1);
      candidates.push(image);
      writeFileSync(image, 'partial');
      return createResult({ status: 1, stderr: busyStderr });
    };
    const execute = (command, args) => {
      assert.notEqual(command, '/usr/bin/hdiutil', 'must not verify an unsuccessful candidate');
      stageApp(command, args);
    };
    assert.throws(() => packageDmg({ bundle, version: '0.1.0', arch: 'x64', icon,
      execute, spawn, wait: ms => delays.push(ms) }), error => error.status === 1 && error.signal === null);
    assert.equal(new Set(candidates).size, 3);
    assert.deepEqual(delays, [1000, 2000]);
    assert.equal(readFileSync(join(bundle, 'dmg/Gizloader_0.1.0_x64.dmg'), 'utf8'), 'stale');
    assert.equal(readFileSync(join(unrelated, 'keep'), 'utf8'), 'owned elsewhere');
    assert.ok(candidates.every(image => !existsSync(image)));
    assert.deepEqual(readdirSync(join(bundle, 'dmg')).filter(name => name.startsWith('.gizloader-dmg-')), []);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

for (const [reason, result] of [
  ['unrelated failure', createResult({ status: 1, stderr: Buffer.from('hdiutil: create failed - No space left on device\n') })],
  ['vague busy text', createResult({ status: 1, stderr: Buffer.from('Resource busy\n') })],
  ['busy message with a suffix', createResult({ status: 1, stderr: Buffer.from('hdiutil: create failed - Resource busy (other command)\n') })],
  ['another exit status', createResult({ status: 2, stderr: busyStderr })],
  ['terminated process', createResult({ status: null, signal: 'SIGKILL', stderr: busyStderr })],
  ['unknown status', createResult({ status: null, stderr: busyStderr })],
  ['spawn error', createResult({ status: 1, stderr: busyStderr, error: Object.assign(new Error('spawn failed'), { code: 'ENOENT' }) })],
  ['timeout', createResult({ status: 1, stderr: busyStderr, error: Object.assign(new Error('timed out'), { code: 'ETIMEDOUT' }) })],
  ['output limit', createResult({ status: 1, stderr: busyStderr, error: Object.assign(new Error('buffer exceeded'), { code: 'ENOBUFS' }) })],
  ['other execution error', createResult({ status: 1, stderr: busyStderr, error: new Error('execution failed') })],
]) {
  test(`does not retry ${reason}`, t => {
    const { root, bundle, icon } = fixture();
    t.mock.method(process.stderr, 'write', () => true);
    try {
      let attempts = 0;
      const spawn = (command, args) => {
        attempts++;
        writeFileSync(args.at(-1), 'partial');
        return result;
      };
      const execute = (command, args) => {
        assert.notEqual(command, '/usr/bin/hdiutil', 'failed creation must not reach verification');
        stageApp(command, args);
      };
      assert.throws(() => packageDmg({ bundle, version: '0.1.0', arch: 'x64', icon,
        execute, spawn, wait() { assert.fail('must not wait before retrying'); } }),
      error => result.error ? error === result.error : error.status === result.status && error.signal === result.signal);
      assert.equal(attempts, 1);
      assert.equal(readFileSync(join(bundle, 'dmg/Gizloader_0.1.0_x64.dmg'), 'utf8'), 'stale');
      assert.deepEqual(readdirSync(join(bundle, 'dmg')).filter(name => name.startsWith('.gizloader-dmg-')), []);
    } finally {
      rmSync(root, { recursive: true, force: true });
    }
  });
}

test('verification failure never retries creation, including after a busy recovery', t => {
  const { root, bundle, icon } = fixture();
  t.mock.method(process.stderr, 'write', () => true);
  try {
    let attempts = 0, verifies = 0;
    const delays = [];
    const failure = new Error('verification failed');
    const spawn = (command, args) => {
      attempts++;
      writeFileSync(args.at(-1), attempts === 1 ? 'partial' : 'complete');
      return createResult(attempts === 1 ? { status: 1, stderr: busyStderr } : {});
    };
    const execute = (command, args) => {
      stageApp(command, args);
      if (command === '/usr/bin/hdiutil') {
        assert.equal(args[0], 'verify');
        verifies++;
        throw failure;
      }
    };
    assert.throws(() => packageDmg({ bundle, version: '0.1.0', arch: 'x64', icon,
      execute, spawn, wait: ms => delays.push(ms) }), error => error === failure);
    assert.equal(attempts, 2);
    assert.equal(verifies, 1);
    assert.deepEqual(delays, [1000]);
    assert.equal(readFileSync(join(bundle, 'dmg/Gizloader_0.1.0_x64.dmg'), 'utf8'), 'stale');
    assert.deepEqual(readdirSync(join(bundle, 'dmg')).filter(name => name.startsWith('.gizloader-dmg-')), []);
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
