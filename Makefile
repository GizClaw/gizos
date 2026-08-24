.DEFAULT_GOAL := help

.PHONY: help cfg-doctor bazel-build bazel-test bazel-test-downstream-consumer bazel-test-mqtt_public_broker_smoke bazel-test-gizclaw_h2peer_live_test bazel-test-gizclaw_pion_live_test bazel-coverage-report bazel-release h2loader-bin test-web guides-build guides-watch guides-preview

BAZEL_BIN ?= bazel
GUIDES_WATCH_HOST ?= 127.0.0.1
GUIDES_WATCH_PORT ?= 5174
GUIDES_PREVIEW_HOST ?= 127.0.0.1
GUIDES_PREVIEW_PORT ?= 4173

export BAZEL_BIN BAZEL_CONFIG RELEASE_SLICE RELEASE_VERSION RELEASE_INPUT_DIR
export RELEASE_STAGING_DIR H2_BAZEL_CONFIG
export BAZEL_REMOTE_CACHE_URL BAZEL_REMOTE_CACHE_MODE
export BAZEL_BUILD_REMOTE_DOWNLOAD_OUTPUTS
export H2_NATIVE_CCACHE_RUNTIME_ROOT
export GUIDES_WATCH_HOST GUIDES_WATCH_PORT GUIDES_PREVIEW_HOST GUIDES_PREVIEW_PORT

help:
	@scripts/config/help.sh

cfg-doctor:
	@scripts/config/cfg-doctor.sh

bazel-build:
	@scripts/bazel/bazel-build.py

bazel-test:
	@scripts/bazel/bazel-test.py

bazel-test-downstream-consumer:
	@scripts/bazel/bazel-test-downstream-consumer.sh

bazel-test-mqtt_public_broker_smoke:
	@scripts/bazel/bazel-test-mqtt_public_broker_smoke.sh

bazel-test-gizclaw_h2peer_live_test:
	@scripts/bazel/bazel-test-gizclaw_h2peer_live_test.sh

bazel-test-gizclaw_pion_live_test:
	@scripts/bazel/bazel-test-gizclaw_pion_live_test.sh

bazel-coverage-report:
	@scripts/bazel/bazel-coverage-report.py

bazel-release:
	@scripts/bazel/bazel-release.py

h2loader-bin:
	@scripts/h2loader/h2loader-bin.sh

test-web:
	@scripts/test/test-web.sh

guides-build:
	@scripts/guides/guides-build.sh

guides-watch:
	@scripts/guides/guides-watch.sh

guides-preview:
	@scripts/guides/guides-preview.sh
