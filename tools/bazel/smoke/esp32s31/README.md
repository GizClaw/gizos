# ESP32-S31 compile and link smoke check

This build-only target verifies the pinned ESP-IDF toolchain, bare-chip board
profile, and cross-language archive linkage. It is owned by build tooling and
is not a project E2E App or a hardware test. It is tagged `manual` and
`compile-only`; do not add it to the device package ledger.
