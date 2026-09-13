# PAL filesystem seek and EOF coverage

The public filesystem case previously wrote and read a whole file only. It now
also checks that a subsequent read returns zero bytes at EOF, then seeks backward
to offset 3 and verifies four bytes against the original payload. The same case
runs in the standalone Filesystem suite selected by the JieLi PAL target.

The executable fixture models a file cursor. Two additional injected failures
prove the assertions reject a successful-but-ignored seek and repeated data at
EOF. Existing directory/type/size failure cases still run and verify cleanup.
Both `pal_e2e_test` and `pref_e2e_test` executed and passed without test caching.
Local log: `tmp/jieli/pal-fs-seek-test.log`.

The JieLi PAL firmware and tar.zlib package built successfully with the native
AC791N Linux toolchain (38.479 seconds). Local build log:
`tmp/jieli/pal-fs-seek-native.log`.

No JieLi provider bug is inferred solely from the missing previous coverage.
Hardware execution remains pending USB DL recovery of the old candidate Loader;
this evidence does not establish seek correctness or complete filesystem coverage
on the board.
