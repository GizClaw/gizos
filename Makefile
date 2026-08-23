.DEFAULT_GOAL := help

.PHONY: help bazel-build bazel-test bazel-test-mqtt_public_broker_smoke bazel-test-gizclaw_h2peer_live_test bazel-test-gizclaw_pion_live_test test-web

BAZEL_BIN ?= bazel

export BAZEL_BIN BAZEL_CONFIG
export BAZEL_REMOTE_CACHE_URL BAZEL_REMOTE_CACHE_MODE
export BAZEL_BUILD_REMOTE_DOWNLOAD_OUTPUTS

help:
	@scripts/config/help.sh

bazel-build:
	@scripts/bazel/bazel-build.py

bazel-test:
	@scripts/bazel/bazel-test.py

bazel-test-mqtt_public_broker_smoke:
	@scripts/bazel/bazel-test-mqtt_public_broker_smoke.sh

bazel-test-gizclaw_h2peer_live_test:
	@scripts/bazel/bazel-test-gizclaw_h2peer_live_test.sh

bazel-test-gizclaw_pion_live_test:
	@scripts/bazel/bazel-test-gizclaw_pion_live_test.sh

test-web:
	@scripts/test/test-web.sh
