import fs from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { pathToFileURL } from 'node:url';
import { parseDocument } from 'yaml';

const keyPattern = /^[a-z0-9][a-z0-9._-]*$/;
const cIdentifierPattern = /^[A-Za-z_][A-Za-z0-9_]*$/;
const printfPattern = /%(?:\d+\$)?[-+ #0']*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|j|z|t|L)?[diuoxXfFeEgGaAcspn]/g;

function fail(message) {
  throw new Error(message);
}

function placeholders(value) {
  return value.replaceAll('%%', '').match(printfPattern) ?? [];
}

function upperIdentifier(value) {
  return value.toUpperCase();
}

export function catalogNames(symbolPrefix) {
  if (!cIdentifierPattern.test(symbolPrefix)) {
    fail(`invalid C symbol prefix: ${symbolPrefix}`);
  }
  const upperPrefix = upperIdentifier(symbolPrefix);
  return {
    headerFile: `${symbolPrefix}_catalog.h`,
    sourceFile: `${symbolPrefix}_catalog.c`,
    headerGuard: `${upperPrefix}_CATALOG_H`,
    tagPrefix: `${upperPrefix}_TAG`,
    registerFunction: `${symbolPrefix}_catalog_register`,
    languagesArray: `s_${symbolPrefix}_languages`,
    tagsArray: `s_${symbolPrefix}_tags`,
    translationsArray: `s_${symbolPrefix}_translations`,
  };
}

function tagMacro(key, tagPrefix) {
  return `${tagPrefix}_${key.toUpperCase().replaceAll(/[^A-Z0-9]+/g, '_')}`;
}

function cString(value) {
  const escapes = new Map([
    ['\u0007', '\\a'],
    ['\b', '\\b'],
    ['\t', '\\t'],
    ['\n', '\\n'],
    ['\u000b', '\\v'],
    ['\f', '\\f'],
    ['\r', '\\r'],
    ['"', '\\"'],
    ['\\', '\\\\'],
  ]);
  let encoded = '';
  for (const character of value) {
    const codePoint = character.codePointAt(0);
    if (codePoint === 0) fail('translations must not contain NUL');
    if (codePoint >= 0x80 && codePoint <= 0x9f) {
      fail('translations must not contain C1 control characters');
    }
    const escaped = escapes.get(character);
    if (escaped !== undefined) {
      encoded += escaped;
    } else if (codePoint < 0x20 || codePoint === 0x7f) {
      encoded += `\\${codePoint.toString(8).padStart(3, '0')}`;
    } else {
      encoded += character;
    }
  }
  return `"${encoded}"`;
}

function translationText(locale, key, value) {
  if (typeof value === 'string') return value;
  if (value === null || Array.isArray(value) || typeof value !== 'object') {
    fail(`${locale}.yml: ${key} must be a string or constrained translation`);
  }
  const fields = Object.keys(value).sort();
  if (JSON.stringify(fields) !== JSON.stringify(['max_characters', 'text'])) {
    fail(`${locale}.yml: ${key} constrained translation must contain exactly text and max_characters`);
  }
  if (!Number.isSafeInteger(value.max_characters) || value.max_characters <= 0) {
    fail(`${locale}.yml: ${key} max_characters must be a positive integer`);
  }
  if (typeof value.text !== 'string') {
    fail(`${locale}.yml: ${key} text must be a string`);
  }
  for (const line of value.text.split('\n')) {
    const length = [...line].length;
    if (length > value.max_characters) {
      fail(`${locale}.yml: ${key} exceeds max_characters=${value.max_characters} with ${length} characters`);
    }
  }
  return value.text;
}

async function loadLocale(inputDirectory, locale) {
  const filename = path.join(inputDirectory, `${locale}.yml`);
  let source;
  try {
    source = new TextDecoder('utf-8', { fatal: true }).decode(
      await fs.readFile(filename),
    );
  } catch (error) {
    fail(`${locale}.yml: must be valid UTF-8: ${error.message}`);
  }
  const document = parseDocument(source, { uniqueKeys: true });
  if (document.errors.length !== 0) {
    fail(`${locale}.yml: ${document.errors.map((error) => error.message).join('; ')}`);
  }
  const value = document.toJS({ mapAsMap: false });
  if (value === null || Array.isArray(value) || typeof value !== 'object') {
    fail(`${locale}.yml: expected one locale root`);
  }
  const roots = Object.keys(value);
  if (roots.length !== 1 || roots[0] !== locale) {
    fail(`${locale}.yml: root must be exactly ${locale}`);
  }
  const sourceCatalog = value[locale];
  if (sourceCatalog === null || Array.isArray(sourceCatalog) ||
      typeof sourceCatalog !== 'object') {
    fail(`${locale}.yml: locale root must contain a key/value mapping`);
  }
  const catalog = {};
  for (const [key, sourceTranslation] of Object.entries(sourceCatalog)) {
    if (!keyPattern.test(key)) fail(`${locale}.yml: invalid key ${key}`);
    const translation = translationText(locale, key, sourceTranslation);
    if (translation.trim().length === 0) {
      fail(`${locale}.yml: ${key} must be a non-empty string`);
    }
    if (translation.includes('\0')) {
      fail(`${locale}.yml: ${key} must not contain NUL`);
    }
    if (/[-]/u.test(translation)) {
      fail(`${locale}.yml: ${key} must not contain C1 control characters`);
    }
    catalog[key] = translation;
  }
  return catalog;
}

export async function loadCatalog(inputDirectory, locales) {
  if (!Array.isArray(locales) || locales.length === 0) {
    fail('at least one locale is required');
  }
  if (new Set(locales).size !== locales.length) {
    fail('locales must not contain duplicates');
  }
  const catalogs = new Map();
  for (const locale of locales) {
    catalogs.set(locale, await loadLocale(inputDirectory, locale));
  }
  const keys = Object.keys(catalogs.get(locales[0])).sort();
  const canonicalSet = new Set(keys);
  for (const locale of locales.slice(1)) {
    const localeKeys = Object.keys(catalogs.get(locale)).sort();
    const missing = keys.filter((key) => !Object.hasOwn(catalogs.get(locale), key));
    const extra = localeKeys.filter((key) => !canonicalSet.has(key));
    if (missing.length !== 0 || extra.length !== 0) {
      fail(`${locale}.yml: key mismatch; missing=[${missing.join(', ')}] extra=[${extra.join(', ')}]`);
    }
  }
  for (const key of keys) {
    const expected = placeholders(catalogs.get(locales[0])[key]);
    for (const locale of locales.slice(1)) {
      const actual = placeholders(catalogs.get(locale)[key]);
      if (JSON.stringify(actual) !== JSON.stringify(expected)) {
        fail(`${locale}.yml: ${key} printf placeholders do not match ${locales[0]}`);
      }
    }
  }
  return { catalogs, keys, locales: [...locales] };
}

export function generateCatalog({ catalogs, keys, locales }, symbolPrefix) {
  const names = catalogNames(symbolPrefix);
  const macros = new Map();
  for (const key of keys) {
    const macro = tagMacro(key, names.tagPrefix);
    if (macros.has(macro)) {
      fail(`tag macro collision: ${key} and ${macros.get(macro)}`);
    }
    macros.set(macro, key);
  }
  const notice = '/* Generated by tools/lvgl/conv_i18n_yaml. Do not edit. */';
  const header = [
    notice,
    `#ifndef ${names.headerGuard}`,
    `#define ${names.headerGuard}`,
    '',
    '#include <stdbool.h>',
    '',
    '#ifdef __cplusplus',
    'extern "C" {',
    '#endif',
    '',
    ...keys.map((key) => `#define ${tagMacro(key, names.tagPrefix)} ${cString(key)}`),
    '',
    `bool ${names.registerFunction}(void);`,
    '',
    '#ifdef __cplusplus',
    '}',
    '#endif',
    '',
    '#endif',
    '',
  ].join('\n');

  const translations = [];
  for (const key of keys) {
    for (const locale of locales) translations.push(catalogs.get(locale)[key]);
  }
  const source = [
    notice,
    `#include "${names.headerFile}"`,
    '',
    '#include "lvgl.h"',
    '',
    `static const char *const ${names.languagesArray}[] = {`,
    ...locales.map((locale) => `    ${cString(locale)},`),
    '    NULL,',
    '};',
    '',
    `static const char *const ${names.tagsArray}[] = {`,
    ...keys.map((key) => `    ${tagMacro(key, names.tagPrefix)},`),
    '    NULL,',
    '};',
    '',
    `static const char *const ${names.translationsArray}[] = {`,
    ...translations.map((translation) => `    ${cString(translation)},`),
    '};',
    '',
    `bool ${names.registerFunction}(void) {`,
    `  return lv_translation_add_static(${names.languagesArray},`,
    `                                   ${names.tagsArray},`,
    `                                   ${names.translationsArray}) != NULL;`,
    '}',
    '',
  ].join('\n');
  return new Map([
    [names.headerFile, header],
    [names.sourceFile, source],
  ]);
}

export async function writeCatalog(outputDirectory, generated) {
  await fs.mkdir(outputDirectory, { recursive: true });
  for (const [filename, content] of generated) {
    await fs.writeFile(path.join(outputDirectory, filename), content, 'utf8');
  }
}

export async function checkCatalog(outputDirectory, generated) {
  const stale = [];
  for (const [filename, content] of generated) {
    const outputPath = path.join(outputDirectory, filename);
    const current = await fs.readFile(outputPath, 'utf8').catch(() => null);
    if (current !== content) stale.push(outputPath);
  }
  if (stale.length !== 0) fail(`generated catalog is stale: ${stale.join(', ')}`);
}

function parseArgs(args) {
  const options = { check: false, locales: [] };
  for (let index = 0; index < args.length; index += 1) {
    const argument = args[index];
    if (argument === '--check') {
      options.check = true;
      continue;
    }
    if (!['--input', '--output', '--locale', '--symbol-prefix'].includes(argument)) {
      fail(`unsupported argument: ${argument}`);
    }
    const value = args[++index];
    if (!value) fail(`${argument} requires a value`);
    if (argument === '--locale') options.locales.push(value);
    else options[argument.slice(2).replace('-', '')] = value;
  }
  for (const name of ['input', 'output', 'symbolprefix']) {
    if (!options[name]) fail(`--${name === 'symbolprefix' ? 'symbol-prefix' : name} is required`);
  }
  if (options.locales.length === 0) fail('at least one --locale is required');
  const invocationRoot = process.env.BUILD_WORKSPACE_DIRECTORY ?? process.cwd();
  return {
    ...options,
    input: path.resolve(invocationRoot, options.input),
    output: path.resolve(invocationRoot, options.output),
  };
}

async function run() {
  const options = parseArgs(process.argv.slice(2));
  const catalog = await loadCatalog(options.input, options.locales);
  const generated = generateCatalog(catalog, options.symbolprefix);
  if (options.check) {
    await checkCatalog(options.output, generated);
  } else {
    await writeCatalog(options.output, generated);
  }
}

if (process.argv[1] && pathToFileURL(path.resolve(process.argv[1])).href === import.meta.url) {
  run().catch((error) => {
    console.error(error.message);
    process.exitCode = 1;
  });
}
