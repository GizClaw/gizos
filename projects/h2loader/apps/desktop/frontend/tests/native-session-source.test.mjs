import test from 'node:test'
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { sessionSourceDigest } from '../../scripts/prepare-native.mjs'

test('Windows-style Git checkout preserves the exact session source digest', () => {
  const repository = resolve(dirname(fileURLToPath(import.meta.url)), '../../../../../..')
  const checkout = mkdtempSync(resolve(tmpdir(), 'h2loader-session-eol-'))
  const names = ['BUILD.bazel', 'session.c', 'session_test.c'].map(name => `projects/h2loader/targets/cc_binary/desktop_session/${name}`)
  const git = args => execFileSync('git', args, { cwd: checkout, stdio: ['ignore', 'pipe', 'pipe'] })
  try {
    mkdirSync(resolve(checkout, 'projects/h2loader/targets/cc_binary/desktop_session'), { recursive: true })
    copyFileSync(resolve(repository, '.gitattributes'), resolve(checkout, '.gitattributes'))
    for (const name of names) copyFileSync(resolve(repository, name), resolve(checkout, name))
    writeFileSync(resolve(checkout, 'control.txt'), 'uncontrolled text\n')
    git(['init', '--quiet'])
    git(['config', 'core.autocrlf', 'true'])
    git(['add', '.'])
    for (const name of [...names, 'control.txt']) rmSync(resolve(checkout, name))
    git(['checkout', '--', ...names, 'control.txt'])
    assert.equal(readFileSync(resolve(checkout, 'control.txt'), 'utf8'), 'uncontrolled text\n')
    for (const name of names) assert.deepEqual(readFileSync(resolve(checkout, name)), readFileSync(resolve(repository, name)))
    assert.equal(sessionSourceDigest(checkout), sessionSourceDigest(repository))
  } finally { rmSync(checkout, { recursive: true, force: true }) }
})
