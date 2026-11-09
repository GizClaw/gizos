"""Versioned locators for JieLi SDK and tool installations."""

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
    for candidate in root.readdir():
        if candidate.basename == ".git":
            continue
        if candidate.is_dir:
            repository_ctx.watch_tree(candidate)
        else:
            repository_ctx.watch(candidate)
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
    if repository_ctx.attr.allow_subdirectory:
        # The locator may name the SDK root inside a larger checkout (for
        # example firmware-devenv's jieli_ac695n_sdk/SDK); identity and
        # cleanliness are still validated on the enclosing Git checkout.
        if not str(root.realpath).startswith(str(actual_root.realpath)):
            fail("%s must stay inside its Git checkout: %s" % (repository_ctx.attr.environment_variable, root))
    elif str(actual_root.realpath) != str(root.realpath):
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
        "allow_subdirectory": attr.bool(default = False),
        "commit_file": attr.label(allow_single_file = True, mandatory = True),
        "environment_variable": attr.string(mandatory = True),
        "kind": attr.string(mandatory = True),
        "required_files": attr.string_list(),
    },
    environ = [
        "JIELI_AC695N_SDK_PATH",
        "JIELI_AC791N_SDK_PATH",
    ],
)

def _jieli_archive_contract(repository_ctx, kind):
    for raw_line in repository_ctx.read(repository_ctx.attr.archives_file).splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split(" ")
        if len(fields) != 5:
            fail("invalid JieLi toolchain archive entry: %s" % raw_line)
        entry_kind, archive, archive_sha256, tree_sha256, directory = fields
        if entry_kind == kind:
            return struct(
                archive = archive,
                archive_sha256 = archive_sha256,
                directory = directory,
                tree_sha256 = tree_sha256,
            )
    fail("JieLi toolchain archive contract is missing the %s entry" % kind)

def _jieli_tools_repository_impl(repository_ctx):
    """Locate one devenv-unpacked JieLi tool tree and validate its identity.

    The upstream pkgman short links always redirect to the newest release and
    keep no version history, so firmware-devenv mirrors the archives and
    unpacks them; this rule only consumes the unpacked root named by the
    environment variable and fails closed when the expanded tree does not
    match the pinned identity.
    """
    kind = repository_ctx.attr.kind
    value = _environment_value(repository_ctx, repository_ctx.attr.environment_variable)
    if not value:
        _write_locator(
            repository_ctx,
            kind,
            False,
            metadata = {"reason": "environment variable is unset"},
        )
        return
    root = repository_ctx.path(value)
    if not root.exists or not root.is_dir:
        fail("%s is not a directory: %s" % (repository_ctx.attr.environment_variable, root))
    contract = _jieli_archive_contract(repository_ctx, repository_ctx.attr.contract_kind)
    for relative in repository_ctx.attr.required_files:
        if not root.get_child(relative).exists:
            fail("%s required input is missing: %s" % (kind, relative))
    python = repository_ctx.which("python3")
    if not python:
        fail("python3 is required to verify the JieLi %s tree" % kind)
    result = repository_ctx.execute([
        python,
        repository_ctx.path(repository_ctx.attr.identity_tool),
        "--directory",
        root,
        "--expected",
        contract.tree_sha256,
    ], quiet = True)
    if result.return_code:
        fail("JieLi %s identity validation failed: %s" % (kind, result.stderr.strip()))
    repository_ctx.watch_tree(root)
    paths = {"root": str(root.realpath)}
    for name, relative in repository_ctx.attr.exported_paths.items():
        paths[name] = str(root.realpath.get_child(relative))
    _write_locator(
        repository_ctx,
        kind,
        True,
        paths = paths,
        metadata = {
            "archive": contract.archive,
            "archive_sha256": contract.archive_sha256,
            "directory": contract.directory,
            "tree_sha256": contract.tree_sha256,
        },
    )

_jieli_tools_repository = repository_rule(
    implementation = _jieli_tools_repository_impl,
    attrs = {
        "archives_file": attr.label(allow_single_file = True, mandatory = True),
        "contract_kind": attr.string(mandatory = True),
        "environment_variable": attr.string(mandatory = True),
        "exported_paths": attr.string_dict(),
        "identity_tool": attr.label(allow_single_file = True, mandatory = True),
        "kind": attr.string(mandatory = True),
        "required_files": attr.string_list(),
    },
    environ = ["JIELI_TOOLCHAIN_ROOT", "JIELI_POSTBUILD_ROOT"],
)

def _extension_impl(_module_ctx):
    _local_sdk_repository(
        name = "h2_jieli_ac695n_sdk",
        # firmware-devenv exports the SDK root (jieli_ac695n_sdk/SDK) inside
        # the mirror checkout.
        allow_subdirectory = True,
        commit_file = "//tools/bazel/native/versions:jieli_ac695n_sdk_commit.txt",
        environment_variable = "JIELI_AC695N_SDK_PATH",
        kind = "jieli-ac695n-sdk",
        required_files = [
            "cpu/br23/sdk_ld.c",
            "cpu/br23/tools/download.c",
            "cpu/br23/tools/download/standard/download.bat",
        ],
    )
    _local_sdk_repository(
        name = "h2_jieli_ac707n_sdk",
        # firmware-devenv exports the SDK root (jieli_ac707n_sdk/SDK) inside
        # the mirror checkout.
        allow_subdirectory = True,
        commit_file = "//tools/bazel/native/versions:jieli_ac707n_sdk_commit.txt",
        environment_variable = "JIELI_AC707N_SDK_PATH",
        kind = "jieli-ac707n-sdk",
        required_files = [
            "cpu/br35/sdk_ld.c",
            "cpu/br35/tools/download.c",
            "cpu/br35/maskrom_stubs.ld",
        ],
    )
    _local_sdk_repository(
        name = "h2_jieli_ac791n_sdk",
        allow_subdirectory = True,
        commit_file = "//tools/bazel/native/versions:jieli_ac791n_sdk_commit.txt",
        environment_variable = "JIELI_AC791N_SDK_PATH",
        kind = "jieli-ac791n-sdk",
        required_files = [
            "cpu/wl82/sdk_ld.c",
            "cpu/wl82/tools/download.c",
            "cpu/wl82/tools/isd_config_rule.c",
        ],
    )
    _jieli_tools_repository(
        name = "h2_jieli_toolchain",
        archives_file = "//tools/bazel/native/versions:jieli_toolchain_archives.txt",
        contract_kind = "toolchain",
        environment_variable = "JIELI_TOOLCHAIN_ROOT",
        exported_paths = {
            "pi32v2_bin": "pi32v2/bin",
            "q32s_bin": "q32s/bin",
        },
        identity_tool = "//tools/bazel/native:toolchain_identity.py",
        kind = "jieli-toolchain",
        required_files = [
            "pi32v2/bin/clang",
            "pi32v2/bin/lto-wrapper",
            "pi32v2/bin/objcopy",
            "pi32v2/bin/objdump",
            "pi32v2/bin/objsizedump",
            "q32s/bin/clang",
        ],
    )
    _jieli_tools_repository(
        name = "h2_jieli_postbuild",
        archives_file = "//tools/bazel/native/versions:jieli_toolchain_archives.txt",
        contract_kind = "postbuild",
        environment_variable = "JIELI_POSTBUILD_ROOT",
        identity_tool = "//tools/bazel/native:toolchain_identity.py",
        kind = "jieli-postbuild",
        required_files = [
            "fw_add",
            "isd_download",
            "pack_res",
            "remove_tailing_zeros",
            "ufw_maker",
        ],
    )

native_repositories = module_extension(implementation = _extension_impl)
