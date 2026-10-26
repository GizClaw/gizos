import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { application, repository, sessionSourceDigest } from './prepare-native.mjs'

const nativeDirectory = join(application, 'src-tauri/binaries')
const manifest = JSON.parse(readFileSync(join(nativeDirectory, 'manifest.json'), 'utf8'))
const pin = {commit: spawnSync('git', ['rev-parse','HEAD'], {cwd: repository, encoding:'utf8'}).stdout.trim()}
assert.equal(manifest.commit, pin.commit, 'Native source differs from the desktop SDK pin')
const suffix = manifest.targetTriple.includes('windows') ? '.exe' : ''
const binary = join(nativeDirectory, `h2loader-${manifest.targetTriple}${suffix}`)
assert.equal(createHash('sha256').update(readFileSync(binary)).digest('hex'), manifest.sha256)

function run(args) {
  const result = spawnSync(binary, ['--no-ble', ...args], {
    cwd: repository, encoding: 'utf8', windowsHide: true, timeout: 30000,
    stdio: ['ignore', 'pipe', 'pipe'],
  })
  if (result.error || result.status !== 0) {
    throw new Error(`Native H2Loader smoke failed (${result.status ?? result.error?.message}): ${(result.stderr || result.stdout || '').slice(-8192)}`)
  }
  return result.stdout
}

assert.match(run(['--help']), /usage: h2loader/)
const session = join(nativeDirectory, `h2loader-desktop-session-${manifest.targetTriple}${suffix}`)
assert.equal(manifest.session.protocol, 1)
assert.equal(manifest.session.sourceSha256, sessionSourceDigest())
assert.equal(createHash('sha256').update(readFileSync(session)).digest('hex'), manifest.session.sha256)
const sessionHelp = spawnSync(session, ['--help'], { encoding: 'utf8', timeout: 30000, windowsHide: true })
assert.equal(sessionHelp.status, 0)
assert.match(sessionHelp.stdout, /h2loader-desktop-session protocol=1/)

// Unix CLI compositions declare /tmp; platform-specific TMPDIR may be outside
// that namespace. Windows maps each accessible local DOS drive.
const directory = mkdtempSync(join(process.platform === 'win32' ? tmpdir() : '/tmp', 'h2loader-native-smoke-'))
try {
  for (const name of ['ascii', '子目录']) {
    const fixture = join(directory, name)
    mkdirSync(fixture)
    const image = join(fixture, name === 'ascii' ? 'image.bin' : '固件.bin')
    writeFileSync(image, Buffer.from([0xe9, 1, 2, 3, 4, 5]))
    for (const version of ['0.1.0', '0.10.7-prod.1+fixture']) {
      const output = join(fixture, `${version}.tar`)
      const stdout = run(['package', '--app-bin', image, '--out', output,
        '--board', 'fixture', '--target', 'host', '--version', version])
      assert.match(stdout, /H2_LOADER_PACKAGE result=OK/)
      const archive = readFileSync(output)
      assert.equal(archive.subarray(257, 262).toString('ascii'), 'ustar')
      const inspected = spawnSync(session, ['--inspect', output], {encoding: 'utf8', timeout: 30000, windowsHide: true})
      assert.equal(inspected.status, 0, inspected.stderr)
      assert.equal(JSON.parse(inspected.stdout).version, version)

    }
  }
} finally {
  rmSync(directory, { recursive: true, force: true })
}
console.log(`Native H2Loader help and ASCII/Unicode package I/O passed (${manifest.targetTriple})`)
