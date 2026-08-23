"""Read-only repository for downloaded per-firmware release artifacts."""

_ENV = "H2_FIRMWARE_RELEASE_INPUT_DIR"


def _repository_impl(repository_ctx):
    source = repository_ctx.os.environ.get(_ENV)
    if source:
        path = repository_ctx.path(source)
        if path.exists:
            repository_ctx.watch_tree(path)
            repository_ctx.symlink(path, "inputs")
    repository_ctx.file(
        "BUILD.bazel",
        """package(default_visibility = [\"//visibility:public\"])

filegroup(
    name = \"all\",
    srcs = glob([\"inputs/**\"], allow_empty = True),
)
""",
    )


_repository = repository_rule(
    implementation = _repository_impl,
    environ = [_ENV],
    local = True,
)


def _extension_impl(module_ctx):
    _repository(name = "h2_firmware_release_inputs")


release_inputs = module_extension(implementation = _extension_impl)
