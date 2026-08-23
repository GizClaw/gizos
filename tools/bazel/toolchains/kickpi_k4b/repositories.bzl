"""Pinned Arm GNU toolchain repository for the KickPi K4B T113-S3."""

_ARCHIVE_SHA256 = "3f9bc7a68f744a5edc7caebff5f3f2c3bc1ff9d8ac8b05f7680a0071461deede"
_ARCHIVE_URL = "https://developer.arm.com/-/media/Files/downloads/gnu-a/8.2-2018.11/gcc-arm-8.2-2018.11-x86_64-arm-linux-gnueabihf.tar.xz"
_STRIP_PREFIX = "gcc-arm-8.2-2018.11-x86_64-arm-linux-gnueabihf"

def _kickpi_k4b_toolchain_repository_impl(repository_ctx):
    repository_ctx.download_and_extract(
        url = _ARCHIVE_URL,
        sha256 = _ARCHIVE_SHA256,
        stripPrefix = _STRIP_PREFIX,
    )
    repository_ctx.file("BUILD.bazel", repository_ctx.read(repository_ctx.attr._build_file))
    repository_ctx.file("cc_toolchain_config.bzl", repository_ctx.read(repository_ctx.attr._config_file))

_kickpi_k4b_toolchain_repository = repository_rule(
    implementation = _kickpi_k4b_toolchain_repository_impl,
    attrs = {
        "_build_file": attr.label(default = "//tools/bazel/toolchains/kickpi_k4b:toolchain.BUILD.bazel"),
        "_config_file": attr.label(default = "//tools/bazel/toolchains/kickpi_k4b:cc_toolchain_config.bzl"),
    },
)

def _kickpi_k4b_toolchain_extension_impl(_module_ctx):
    _kickpi_k4b_toolchain_repository(name = "k4b_arm_gnu_toolchain")

kickpi_k4b_toolchain = module_extension(implementation = _kickpi_k4b_toolchain_extension_impl)
