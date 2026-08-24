"""Versioned locators for the BK7258 SDK and shared Arm toolchain."""

_LOCATOR_SCHEMA = "h2.native-locator.v1"

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

def _read_identity(repository_ctx, label, description):
    value = repository_ctx.read(label).strip()
    invalid = len(value) != 40
    for index in range(len(value)):
        if value[index] not in "0123456789abcdef":
            invalid = True
    if invalid:
        fail("invalid %s commit: %r" % (description, value))
    return value

def _git(repository_ctx):
    git = repository_ctx.which("git")
    if not git:
        fail("git is required to validate native SDK repositories")
    return git

def _git_output(repository_ctx, git, root, arguments, description):
    result = repository_ctx.execute(
        [git, "-C", str(root)] + arguments,
        quiet = True,
    )
    if result.return_code:
        fail("cannot validate %s: %s" % (description, result.stderr.strip()))
    return result.stdout.strip()

def _watch_git_state(repository_ctx, root):
    repository_ctx.watch_tree(root, exclude = [".git/**"])
    dot_git = root.get_child(".git")
    if not dot_git.exists:
        return
    if dot_git.is_dir:
        for relative in ["HEAD", "index", "packed-refs"]:
            candidate = dot_git.get_child(relative)
            if candidate.exists:
                repository_ctx.watch(candidate)
    else:
        repository_ctx.watch(dot_git)

def _local_sdk_repository_impl(repository_ctx):
    value = _environment_value(repository_ctx, repository_ctx.attr.environment_variable)
    if not value:
        _write_locator(
            repository_ctx,
            repository_ctx.attr.kind,
            False,
            metadata = {"reason": "environment variable is unset"},
        )
        return

    root = repository_ctx.path(value)
    if not root.exists or not root.is_dir:
        fail("%s is not a directory: %s" % (repository_ctx.attr.environment_variable, root))
    expected = _read_identity(
        repository_ctx,
        repository_ctx.attr.commit_file,
        repository_ctx.attr.kind,
    )
    git = _git(repository_ctx)
    actual_root = repository_ctx.path(_git_output(
        repository_ctx,
        git,
        root,
        ["rev-parse", "--show-toplevel"],
        repository_ctx.attr.kind,
    ))
    if str(actual_root.realpath) != str(root.realpath):
        fail("%s must point to the Git root: %s" % (repository_ctx.attr.environment_variable, root))
    actual = _git_output(
        repository_ctx,
        git,
        root,
        ["rev-parse", "HEAD"],
        repository_ctx.attr.kind,
    )
    if actual != expected:
        fail("%s commit mismatch: expected %s, found %s" % (repository_ctx.attr.kind, expected, actual))
    dirty = _git_output(
        repository_ctx,
        git,
        root,
        ["status", "--porcelain=v1", "--untracked-files=no"],
        repository_ctx.attr.kind,
    )
    if dirty:
        fail("%s has tracked modifications" % repository_ctx.attr.kind)
    for relative in repository_ctx.attr.required_files:
        if not root.get_child(relative).exists:
            fail("%s required input is missing: %s" % (repository_ctx.attr.kind, relative))
    _watch_git_state(repository_ctx, actual_root)
    _write_locator(
        repository_ctx,
        repository_ctx.attr.kind,
        True,
        paths = {
            "checkout": str(actual_root.realpath),
            "root": str(root.realpath),
        },
        metadata = {"commit": expected},
    )

_local_sdk_repository = repository_rule(
    implementation = _local_sdk_repository_impl,
    attrs = {
        "commit_file": attr.label(allow_single_file = True, mandatory = True),
        "environment_variable": attr.string(mandatory = True),
        "kind": attr.string(mandatory = True),
        "required_files": attr.string_list(),
    },
    environ = ["BK7258_PATH"],
)

def _archive_contract(repository_ctx):
    contracts = {}
    for raw_line in repository_ctx.read(repository_ctx.attr.archives_file).splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split(" ")
        if len(fields) != 5:
            fail("invalid BK toolchain archive entry: %s" % raw_line)
        host, url, archive_sha256, tree_sha256, strip_prefix = fields
        contracts[host] = struct(
            archive_sha256 = archive_sha256,
            strip_prefix = strip_prefix,
            tree_sha256 = tree_sha256,
            url = url,
        )
    return contracts

def _arm_host(repository_ctx):
    name = repository_ctx.os.name.lower()
    arch = repository_ctx.os.arch.lower()
    if name == "linux" and arch in ("amd64", "x86_64"):
        return "linux_x86_64"
    if name in ("mac os x", "macos") and arch in ("amd64", "x86_64"):
        return "macos_x86_64"
    if name in ("mac os x", "macos") and arch in ("aarch64", "arm64"):
        probe = repository_ctx.execute(["/usr/bin/arch", "-x86_64", "/usr/bin/true"], quiet = True)
        if probe.return_code:
            fail("UNSUPPORTED: macOS arm64 BK builds require Rosetta 2")
        return "macos_x86_64"
    return None

def _arm_toolchain_repository_impl(repository_ctx):
    host = _arm_host(repository_ctx)
    if not host:
        _write_locator(
            repository_ctx,
            "bk-arm-toolchain",
            False,
            metadata = {"reason": "unsupported host"},
        )
        return
    contract = _archive_contract(repository_ctx).get(host)
    if not contract:
        fail("BK toolchain archive contract is missing for %s" % host)
    repository_ctx.download_and_extract(
        url = contract.url,
        output = "toolchain",
        sha256 = contract.archive_sha256,
        stripPrefix = contract.strip_prefix,
    )
    python = repository_ctx.which("python3")
    if not python:
        fail("python3 is required to verify the downloaded BK toolchain")
    result = repository_ctx.execute([
        python,
        repository_ctx.path(repository_ctx.attr.identity_tool),
        "--directory",
        repository_ctx.path("toolchain"),
        "--expected",
        contract.tree_sha256,
    ], quiet = True)
    if result.return_code:
        fail("downloaded BK toolchain identity validation failed: %s" % result.stderr.strip())
    root = repository_ctx.path("toolchain")
    _write_locator(
        repository_ctx,
        "bk-arm-toolchain",
        True,
        paths = {
            "bin": str(root.get_child("bin")),
            "root": str(root),
        },
        metadata = {
            "archive_sha256": contract.archive_sha256,
            "host": host,
            "tree_sha256": contract.tree_sha256,
            "version": "10.3.1",
        },
    )
    repository_ctx.file(
        "BUILD.bazel",
        """package(default_visibility = [\"//visibility:public\"])

exports_files([\"locator.json\"])
filegroup(name = "all_files", srcs = glob([\"toolchain/**\"], allow_empty = False))
""",
    )

_arm_toolchain_repository = repository_rule(
    implementation = _arm_toolchain_repository_impl,
    attrs = {
        "archives_file": attr.label(allow_single_file = True, mandatory = True),
        "identity_tool": attr.label(allow_single_file = True, mandatory = True),
    },
)

def _extension_impl(_module_ctx):
    _local_sdk_repository(
        name = "gizos_bk7258_sdk",
        commit_file = "//tools/bazel:native_versions/bk7258_sdk_commit.txt",
        environment_variable = "BK7258_PATH",
        kind = "bk7258-sdk",
        required_files = ["Makefile"],
    )
    _arm_toolchain_repository(
        name = "gizos_bk_arm_toolchain",
        archives_file = "//tools/bazel:native_versions/bk_toolchain_archives.txt",
        identity_tool = "//tools/bazel:toolchain_identity.py",
    )

bk_repositories = module_extension(implementation = _extension_impl)
