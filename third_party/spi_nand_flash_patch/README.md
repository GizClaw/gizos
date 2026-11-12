# SPI NAND usage extension

The native build adds a read-only Dhara usage query to Espressif `spi_nand_flash` 1.4.4. The extension compiles a generated copy of the pinned `dhara_glue.c` with the package-local fragment appended, preserving the original managed component and SDK installation. CMake rejects a different upstream source hash.

The query holds the existing device mutex and returns mapped logical pages and usable FTL capacity in bytes. It does not scan, sync, collect garbage, erase, format, or write the medium. FTL metadata, spare blocks, stale physical pages, and filesystem free space are not counted as logical user data. Callers must keep the initialized NAND handle alive until the query returns.
