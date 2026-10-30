"""Explicit offline App encryption entrypoint, also usable after bazel build."""

from projects.h2loader.tools.bazel.firmware_output import encrypted_bk_app_main


if __name__ == "__main__":
    raise SystemExit(encrypted_bk_app_main())
