# BK Preferences storage backup

This directed, manual managed App reads the existing 128 KiB FlashDB physical
partition before opening Preferences or initializing a new database. It emits
ordered 64-byte hex chunks and a SHA256 digest through the native SDK console.
It does not format or erase the region, and it is not an E2E qualification.

Capture the raw console privately: it contains stored user data. Require the
directed board UID, exact package/image binding, a fresh platform boot, every
ordered chunk, and the final matching digest before using the snapshot. The
managed install changes ordinary Loader/P2 metadata; application Preferences
and the reserved tail are the compatibility sample. Keep the original App,
Loader, and coredump separately for restoration.

Build `:package` with `--config=bk7258` and an independent firmware version.
Use the task's exclusive managed UART owner for staging and capture.
