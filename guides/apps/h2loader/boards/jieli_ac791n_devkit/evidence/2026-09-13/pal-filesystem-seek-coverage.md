# PAL filesystem seek and EOF coverage

The public filesystem case previously wrote and read a whole file only. It now also checks that a subsequent read returns zero bytes at EOF, then seeks backward to offset 3 and verifies four bytes against the original payload. The same case runs in the standalone Filesystem suite selected by the JieLi PAL target.

The executable fixture models a file cursor. Two additional injected failures prove the assertions reject a successful-but-ignored seek and repeated data at EOF. Existing directory/type/size failure cases still run and verify cleanup. Both `pal_e2e_test` and `pref_e2e_test` executed and passed without test caching. Local log: `tmp/jieli/pal-fs-seek-test.log`.

The JieLi PAL firmware and tar.zlib package built successfully with the native AC791N Linux toolchain (38.479 seconds). Local build log: `tmp/jieli/pal-fs-seek-native.log`.

## Cross-provider execution at e1636d46

Both existing Web targets were executed with `--nocache_test_results`:

- `//projects/e2e/targets/pkg_tar/pal:pal_wasm_test`: passed (Core suite only; this is not filesystem evidence).
- `//projects/e2e/targets/pkg_tar/pal:browser_test`: real Chromium loaded the archive with `?suite=browser`; 12/12 cases passed, including filesystem case 13 (`rc=0`). Final cleanup and teardown reported `cleanup=0`, `fs=0`, `destroy=0`. This exercises the new EOF/backward-seek assertions on Web PAL.

Local command logs: `tmp/jieli/pal-seek-wasm.log` and `tmp/jieli/pal-seek-browser.log`. The actual browser ledger was inspected in `bazel-testlogs/projects/e2e/targets/pkg_tar/pal/browser_test/test.log`. Neither result substitutes for JieLi hardware execution.

No JieLi provider bug is inferred solely from the missing previous coverage. Hardware execution remains pending USB DL recovery of the old candidate Loader; this evidence does not establish seek correctness or complete filesystem coverage on the board.
