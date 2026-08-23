#!/bin/sh
set -eu

exec bazel test \
  //tools/bazel:web_archive_server_test \
  //projects/example/targets/pkg_tar/tap-reset/... \
  //projects/e2e/targets/pkg_tar/libco:archive_test \
  //projects/e2e/targets/pkg_tar/libco:libco_wasm_test \
  //projects/e2e/targets/pkg_tar/pal:archive_test \
  //projects/e2e/targets/pkg_tar/pal:pal_wasm_test \
  //projects/e2e/targets/pkg_tar/h2loader-serial:archive_test \
  //projects/e2e/targets/pkg_tar/h2loader-serial:h2loader_serial_wasm_test \
  //projects/e2e/targets/pkg_tar/h2loader-serial:run_poller_test \
  //projects/e2e/targets/pkg_tar/h2loader-serial:shutdown_test \
  //libs/h2loader_host:h2loader_host_package_test \
  //projects/h2loader/libs/web:h2loader_web \
  //projects/h2loader/apps/batch-loader/app:unit_test \
  //projects/h2loader/apps/batch-loader/app:playwright_test \
  //projects/h2loader/targets/pkg_tar/batch-loader:archive_test
