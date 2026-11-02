import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { createHash } from 'node:crypto';
import { GizloaderBootstrap } from '../../gizos/src/gizloader';

if (process.platform !== 'darwin') throw new Error('The DMG install smoke check requires macOS');
const root = await mkdtemp(join(tmpdir(), 'gizloader-install-check-'));
try {
  const directory = resolve(import.meta.dir, '../release');
  const metadata = JSON.parse(await readFile(join(directory, 'gizloader-darwin-arm64.metadata.json'), 'utf8'));
  // Local build metadata is adapted only inside this disposable offline fixture.
  // It is never published or presented as notarized release metadata.
  const index = { ...metadata, channel: metadata.channel === 'build' ? 'release' : metadata.channel };
  const archive = join(directory, index.artifacts[0].file);
  const indexPath = join(root, 'gizloader-index.json');
  const checksums = join(root, 'SHA256SUMS');
  const text = JSON.stringify(index) + '\n';
  await writeFile(indexPath, text);
  await writeFile(checksums, `${createHash('sha256').update(text).digest('hex')}  gizloader-index.json\n${index.artifacts[0].sha256}  ${index.artifacts[0].file}\n`);
  const bootstrap = new GizloaderBootstrap({ root: join(root, 'store'), home: join(root, 'home') });
  await mkdir(join(root, 'home'));
  const installed = await bootstrap.install({ index: indexPath, checksums, archive, snapshot: index.channel === 'snapshot' });
  const help = Bun.spawn([installed.shim, '--help'], { stdout: 'pipe', stderr: 'pipe' });
  const [stdout, stderr, code] = await Promise.all([
    new Response(help.stdout).text(), new Response(help.stderr).text(), help.exited,
  ]);
  if (code !== 0 || !stdout.includes('usage: gizloader') || !stdout.includes('scan')) {
    throw new Error(`Installed CLI help failed (${code}): ${stderr}`);
  }
  const failed = Bun.spawn([installed.shim, 'unknown-command'], { stdout: 'pipe', stderr: 'pipe' });
  await Promise.all([new Response(failed.stdout).text(), new Response(failed.stderr).text()]);
  if (await failed.exited !== 2) throw new Error('Installed command did not preserve native usage exit code 2');
  console.log('Real DMG import, bundle/version checks, user launcher, CLI output and native exit status passed.');
} finally {
  await rm(root, { recursive: true, force: true });
}
