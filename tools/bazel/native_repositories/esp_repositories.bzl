"""Versioned locators for the public ESP-IDF SDK and tools."""

_LOCATOR_SCHEMA = "gizos.native-locator.v1"

def _write_locator(repository_ctx, kind, enabled, paths = {}, metadata = {}):
    repository_ctx.file(
        "locator.json",
        json.encode_indent({
            "enabled": enabled,
            "kind": kind,
            "metadata": metadata,
            "paths": paths,
            "schema": _LOCATOR_SCHEMA,
        }) + "\n",
    )
    repository_ctx.file(
        "BUILD.bazel",
        """package(default_visibility = [\"//visibility:public\"])

exports_files([\"locator.json\"])
""",
    )

def _environment_value(repository_ctx, name):
    return repository_ctx.os.environ.get(name, "").strip()

def _git_output(repository_ctx, root, arguments, description):
    git = repository_ctx.which("git")
    if not git:
        fail("git is required to validate %s" % description)
    result = repository_ctx.execute([git, "-C", str(root)] + arguments, quiet = True)
    if result.return_code:
        fail("cannot validate %s: %s" % (description, result.stderr.strip()))
    return result.stdout.strip()

def _esp_sdk_repository_impl(repository_ctx):
    value = _environment_value(repository_ctx, "IDF_PATH")
    if not value:
        _write_locator(repository_ctx, "esp-idf-sdk", False, metadata = {"reason": "IDF_PATH is unset"})
        return
    root = repository_ctx.path(value)
    if not root.get_child("tools").get_child("idf.py").exists:
        fail("IDF_PATH is not an ESP-IDF checkout: %s" % root)
    if not root.get_child("tools").get_child("cmake").get_child("version.cmake").exists:
        fail("ESP-IDF version contract is unavailable: %s" % root)
    expected = repository_ctx.read(repository_ctx.attr.commit_file).strip()
    actual_root = repository_ctx.path(_git_output(repository_ctx, root, ["rev-parse", "--show-toplevel"], "ESP-IDF SDK"))
    if str(actual_root.realpath) != str(root.realpath):
        fail("IDF_PATH must point to the ESP-IDF Git root: %s" % root)
    actual = _git_output(repository_ctx, root, ["rev-parse", "HEAD"], "ESP-IDF SDK")
    if actual != expected:
        fail("ESP-IDF commit mismatch: expected %s, found %s" % (expected, actual))
    dirty = _git_output(repository_ctx, root, ["status", "--porcelain=v1", "--untracked-files=no"], "ESP-IDF SDK")
    if dirty:
        fail("ESP-IDF checkout has tracked modifications")
    repository_ctx.watch_tree(root)
    _write_locator(
        repository_ctx,
        "esp-idf-sdk",
        True,
        paths = {"checkout": str(root.realpath), "root": str(root.realpath)},
        metadata = {"commit": expected},
    )

_esp_sdk_repository = repository_rule(
    implementation = _esp_sdk_repository_impl,
    attrs = {"commit_file": attr.label(allow_single_file = True, mandatory = True)},
    environ = ["IDF_PATH"],
)

def _esp_tools_repository_impl(repository_ctx):
    value = _environment_value(repository_ctx, "IDF_TOOLS_PATH")
    if not value:
        _write_locator(repository_ctx, "esp-idf-tools", False, metadata = {"reason": "IDF_TOOLS_PATH is unset"})
        return
    root = repository_ctx.path(value)
    if not root.get_child("tools").exists:
        fail("IDF_TOOLS_PATH is not an ESP-IDF tools directory: %s" % root)
    _write_locator(repository_ctx, "esp-idf-tools", True, paths = {"tools_root": str(root.realpath)})

_esp_tools_repository = repository_rule(
    implementation = _esp_tools_repository_impl,
    environ = ["IDF_TOOLS_PATH"],
)

def _extension_impl(_module_ctx):
    _esp_sdk_repository(
        name = "gizos_esp_idf_sdk",
        commit_file = "//tools/bazel:native_versions/esp_idf_commit.txt",
    )
    _esp_tools_repository(name = "gizos_esp_idf_tools")

esp_repositories = module_extension(implementation = _extension_impl)
