import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {
  checkCatalog,
  generateCatalog,
  loadCatalog,
  writeCatalog,
} from '../src/conv_i18n_yaml.mjs';

const locales = ['zh-CN', 'en-US'];
const symbolPrefix = 'sample_i18n';

function loadFixture(directory) {
  return loadCatalog(directory, locales);
}

function generateFixture(catalog) {
  return generateCatalog(catalog, symbolPrefix);
}

async function fixture(zh, en) {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'lvgl-i18n-'));
  await fs.writeFile(path.join(directory, 'zh-CN.yml'), zh);
  await fs.writeFile(path.join(directory, 'en-US.yml'), en);
  return directory;
}

test('generates deterministic row-major catalog and escapes C strings', async () => {
  const directory = await fixture(
    'zh-CN:\n  value.count: "数量 %u\\n\\\"\\b\\f\\v\\a\\x01\\x7f"\n',
    'en-US:\n  value.count: "Count %u\\n\\\"\\b\\f\\v\\a\\x01\\x7f"\n',
  );
  const catalog = await loadFixture(directory);
  const first = generateFixture(catalog);
  const second = generateFixture(catalog);
  assert.deepEqual(first, second);
  const source = first.get('sample_i18n_catalog.c');
  assert.match(source, /"数量 %u\\n\\\"\\b\\f\\v\\a\\001\\177"/);
  assert.ok(source.indexOf('"数量') < source.indexOf('"Count'));
});

test('rejects NUL and C1 control characters', async () => {
  const nul = await fixture(
    'zh-CN:\n  value: "bad\\0value"\n',
    'en-US:\n  value: value\n',
  );
  await assert.rejects(loadFixture(nul), /must not contain NUL/);

  const c1 = await fixture(
    'zh-CN:\n  value: "bad\\x85value"\n',
    'en-US:\n  value: value\n',
  );
  await assert.rejects(
    loadFixture(c1),
    /must not contain C1 control characters/,
  );
});

test('rejects malformed YAML and a mismatched locale root', async () => {
  const malformed = await fixture(
    'zh-CN:\n  duplicate: one\n  duplicate: two\n',
    'en-US:\n  duplicate: one\n',
  );
  await assert.rejects(loadFixture(malformed), /Map keys must be unique|duplicate/i);
  const wrongRoot = await fixture('zh:\n  value: one\n', 'en-US:\n  value: one\n');
  await assert.rejects(loadFixture(wrongRoot), /root must be exactly zh-CN/);
});

test('rejects invalid UTF-8 input', async () => {
  const directory = await fixture(
    'zh-CN:\n  value: one\n',
    'en-US:\n  value: one\n',
  );
  await fs.writeFile(path.join(directory, 'zh-CN.yml'), Buffer.from([0xff]));
  await assert.rejects(loadFixture(directory), /must be valid UTF-8/);
});

test('rejects missing, extra, and empty translations', async () => {
  const mismatch = await fixture(
    'zh-CN:\n  one: 一\n  two: 二\n',
    'en-US:\n  one: one\n  three: three\n',
  );
  await assert.rejects(loadFixture(mismatch), /key mismatch/);
  const empty = await fixture('zh-CN:\n  one: ""\n', 'en-US:\n  one: one\n');
  await assert.rejects(loadFixture(empty), /must be a non-empty string/);
});

test('accepts constrained translations and rejects text beyond the limit', async () => {
  const valid = await fixture(
    'zh-CN:\n  menu.contact: 联系人\n',
    'en-US:\n  menu.contact:\n    text: Contacts\n    max_characters: 8\n',
  );
  const catalog = await loadFixture(valid);
  assert.equal(catalog.catalogs.get('en-US')['menu.contact'], 'Contacts');

  const tooLong = await fixture(
    'zh-CN:\n  menu.contact: 联系人\n',
    'en-US:\n  menu.contact:\n    text: Guardian Contacts\n    max_characters: 14\n',
  );
  await assert.rejects(loadFixture(tooLong), /exceeds max_characters=14/);

  const invalidSchema = await fixture(
    'zh-CN:\n  menu.contact: 联系人\n',
    'en-US:\n  menu.contact:\n    text: Contacts\n    max_characters: 8\n    width: 180\n',
  );
  await assert.rejects(loadFixture(invalidSchema), /must contain exactly/);
});

test('rejects printf placeholder mismatches', async () => {
  const directory = await fixture(
    'zh-CN:\n  value: "值 %u / %s"\n',
    'en-US:\n  value: "Value %s / %u"\n',
  );
  await assert.rejects(loadFixture(directory), /printf placeholders/);
});

test('detects stale or missing committed generated output', async () => {
  const directory = await fixture(
    'zh-CN:\n  value: one\n',
    'en-US:\n  value: one\n',
  );
  const output = await fs.mkdtemp(path.join(os.tmpdir(), 'lvgl-i18n-output-'));
  const generated = generateFixture(await loadFixture(directory));
  await writeCatalog(output, generated);
  await checkCatalog(output, generated);
  await fs.writeFile(path.join(output, 'sample_i18n_catalog.c'), 'stale\n');
  await assert.rejects(checkCatalog(output, generated), /generated catalog is stale/);
  await fs.rm(path.join(output, 'sample_i18n_catalog.h'));
  await assert.rejects(checkCatalog(output, generated), /generated catalog is stale/);
});
