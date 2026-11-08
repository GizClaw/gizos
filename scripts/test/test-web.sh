#!/bin/sh
set -eu

exec bazel test \
  //tools/bazel/web:web_archive_server_test \
  //projects/testing/targets/pkg_tar/lua-runtime/... \
  //projects/testing/targets/pkg_tar/pal-core:browser_test \
  //projects/testing/targets/pkg_tar/pal-storage/... \
  //projects/testing/targets/pkg_tar/pal-http/... \
  //projects/testing/targets/pkg_tar/pal-wifi/... \
  //projects/testing/targets/pkg_tar/atomic/... \
  //projects/testing/targets/pkg_tar/h2loader-serial:archive_test \
  //projects/testing/targets/pkg_tar/h2loader-serial:h2loader_serial_wasm_test \
  //projects/testing/targets/pkg_tar/h2loader-serial:run_poller_test \
  //projects/testing/targets/pkg_tar/h2loader-serial:shutdown_test \
  //libs/h2loader/host:h2loader_host_package_test \
  //projects/h2loader/libs/web:web \
  //projects/h2loader/libs/web:h2loader_ports_test \
  //projects/h2loader/targets/npm_package/h2loader:wasm_url_test
