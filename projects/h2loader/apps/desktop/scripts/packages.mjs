import { createHash } from 'node:crypto'
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs'
import { join, resolve } from 'node:path'
import { execFileSync } from 'node:child_process'
import { application, repository, sessionSourceDigest } from './prepare-native.mjs'

const root = repository
const read = path => JSON.parse(readFileSync(path, 'utf8'))
const config = read(join(application, 'src-tauri/tauri.conf.json'))
const contract = read(join(application, 'release-contract.json'))
const sdk = { repository: 'https://github.com/GizClaw/gizos.git', commit: execFileSync('git', ['rev-parse', 'HEAD'], {cwd: root, encoding: 'utf8'}).trim(), target: '//projects/h2loader/targets/cc_binary/cli:h2loader' }
const sessionAdapter = { protocol: 1, source_sha256: sessionSourceDigest() }
const version = config.version
if (!/^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$/.test(version)) throw new Error('Desktop version must be a plain SemVer')
const tag = `h2loader-desktop-v${version}`
const commit = execFileSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim()
const output = resolve(process.argv[3] || join(root, 'dist/desktop'))
const digest = path => createHash('sha256').update(readFileSync(path)).digest('hex')
const assetName = target => `${contract.product}_${version}_${target.suffix}`
mkdirSync(output, { recursive: true })

if (process.argv[2] === 'collect') {
  const triple = process.env.H2LOADER_DESKTOP_TARGET || execFileSync('rustc', ['--print', 'host-tuple'], { encoding: 'utf8' }).trim()
  const target = contract.targets.find(item => item.triple === triple)
  if (!target) throw new Error(`Unsupported release target: ${triple}`)
  const metadata = JSON.parse(execFileSync('cargo', ['metadata', '--no-deps', '--format-version', '1', '--manifest-path', join(application, 'src-tauri/Cargo.toml')], { cwd: root, encoding: 'utf8' }))
  if (process.argv[4] && process.argv[4] !== triple) throw new Error('Bundle target differs from the native SDK target')
  const bundle = join(metadata.target_directory, process.argv[4] || '', 'release/bundle', target.format === 'dmg' ? 'dmg' : 'nsis')
  const candidates = readdirSync(bundle).filter(name => name.endsWith(`.${target.format}`))
  if (candidates.length !== 1) throw new Error('Expected exactly one desktop installer in the bundle directory')
  const path = join(output, assetName(target))
  copyFileSync(join(bundle, candidates[0]), path)
  const native = read(join(application, 'src-tauri/binaries/manifest.json'))
  if (native.commit !== sdk.commit || native.targetTriple !== triple || native.session?.sourceSha256 !== sessionAdapter.source_sha256 || native.session?.protocol !== 1) throw new Error('Native SDK provenance does not match the release target')
  writeFileSync(join(output, `${target.platform}.json`), JSON.stringify({
    repository: contract.repository, version, commit, native_sdk: sdk, session_adapter: sessionAdapter,
    asset: { name: assetName(target), platform: target.platform, arch: target.arch, format: target.format,
      size: statSync(path).size, sha256: digest(path), signing: target.signing, notarized: false },
  }, null, 2) + '\n')
} else if (process.argv[2] === 'assemble' || process.argv[2] === 'verify') {
  const assets = contract.targets.map(target => {
    const entry = read(join(output, `${target.platform}.json`))
    const path = join(output, assetName(target))
    if (entry.repository !== contract.repository || entry.version !== version || entry.commit !== commit
      || JSON.stringify(entry.native_sdk) !== JSON.stringify(sdk) || JSON.stringify(entry.session_adapter) !== JSON.stringify(sessionAdapter) || entry.asset.name !== assetName(target)
      || entry.asset.platform !== target.platform || entry.asset.arch !== target.arch || entry.asset.format !== target.format
      || entry.asset.signing !== target.signing || entry.asset.notarized !== false
      || !existsSync(path) || statSync(path).size !== entry.asset.size || digest(path) !== entry.asset.sha256) {
      throw new Error(`Desktop installer ${target.platform} failed release verification`)
    }
    return entry.asset
  })
  const index = { format: 1, repository: contract.repository, release_tag: tag, product: contract.product,
    version, identifier: config.identifier, commit, native_sdk: sdk, session_adapter: sessionAdapter, assets }
  const body = JSON.stringify(index, null, 2) + '\n'
  const indexPath = join(output, 'desktop-index.json')
  if (process.argv[2] === 'assemble') writeFileSync(indexPath, body)
  else if (readFileSync(indexPath, 'utf8') !== body) throw new Error('Desktop index differs from verified installer metadata')
  const sums = ['desktop-index.json', ...assets.map(item => item.name)].sort()
    .map(name => `${digest(join(output, name))}  ${name}\n`).join('')
  if (process.argv[2] === 'assemble') writeFileSync(join(output, 'SHA256SUMS'), sums)
  else if (readFileSync(join(output, 'SHA256SUMS'), 'utf8') !== sums) throw new Error('Desktop checksums differ from installer bytes')
} else throw new Error('Usage: desktop-packages.mjs collect|assemble|verify [output-directory]')
