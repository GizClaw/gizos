.DEFAULT_GOAL := help

.PHONY: help bazel-build bazel-test

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
