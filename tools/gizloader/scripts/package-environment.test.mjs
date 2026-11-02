import assert from 'node:assert/strict';
import test from 'node:test';
import { tauriEnvironment } from './package-environment.mjs';

test('macOS artifact builds explicitly select ad-hoc signing for empty or absent identities', () => {
  for (const environment of [{}, { APPLE_SIGNING_IDENTITY: '' }]) {
    assert.equal(tauriEnvironment(environment, 'darwin').APPLE_SIGNING_IDENTITY, '-');
  }
});

test('Developer ID builds retain the configured identity and caller environment', () => {
  const environment = { APPLE_SIGNING_IDENTITY: 'Developer ID Application: Example (ABCDEFGHIJ)', PATH: '/tools/bin' };
  assert.deepEqual(tauriEnvironment(environment, 'darwin'), environment);
  assert.notEqual(tauriEnvironment(environment, 'darwin'), environment);
});

test('notarization mapping keeps the parent key path and sends Tauri its key ID', () => {
  const environment = {
    APPLE_API_KEY: '/private/tmp/signing/AuthKey_TEST.p8',
    APPLE_API_KEY_ID: 'ABCDEFGHIJ',
    APPLE_API_ISSUER: 'issuer-fixture',
  };
  const child = tauriEnvironment(environment, 'darwin');
  assert.equal(child.APPLE_API_KEY, 'ABCDEFGHIJ');
  assert.equal(child.APPLE_API_KEY_PATH, environment.APPLE_API_KEY);
  assert.equal(child.APPLE_API_ISSUER, environment.APPLE_API_ISSUER);
  assert.equal(environment.APPLE_API_KEY, '/private/tmp/signing/AuthKey_TEST.p8');
  assert.equal(environment.APPLE_API_KEY_PATH, undefined);
});

test('Windows environment is copied without changing Apple configuration', () => {
  const environment = { APPLE_SIGNING_IDENTITY: '', APPLE_API_KEY: '/unused/path', PATH: 'C:\\tools' };
  assert.deepEqual(tauriEnvironment(environment, 'win32'), environment);
});
