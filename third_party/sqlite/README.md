# SQLite Amalgamation

This directory vendors the SQLite C amalgamation for desktop and desktop-test
preference storage only.

- Version: 3.53.3
- Release date: 2026-06-26
- Source archive: https://sqlite.org/2026/sqlite-amalgamation-3530300.zip
- Files committed from the archive:
  - `sqlite3.c`
  - `sqlite3.h`
  - `sqlite3ext.h`

SQLite is public domain. See the SQLite project copyright page for the
canonical license statement:

https://sqlite.org/copyright.html

Update procedure:

1. Download the selected upstream `sqlite-amalgamation-<version>.zip` archive.
2. Replace `sqlite3.c`, `sqlite3.h`, and `sqlite3ext.h` from that archive.
3. Update this README with the version, release date, and source archive URL.
4. Run `bazel test --config=macos_arm64 //libs/pal/providers/sqlite:all` to
   validate the desktop preference backend.

Do not include SQLite from public H2 platform headers, ESP components, BK
components, or product app code. It is a desktop/test implementation dependency.
