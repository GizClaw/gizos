#!/bin/sh
set -eu

"${BAZEL_BIN:-bazel}" test //guides/...
mkdir -p build/guides
find build/guides -type d -exec chmod u+w {} +
rsync --archive --delete --chmod=Du+w bazel-bin/guides/.vitepress/dist/ build/guides/
