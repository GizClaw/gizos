"""Versioned locators for native SDKs, tools, and compiler-cache runtime."""

_LOCATOR_SCHEMA = "h2.native-locator.v1"
_FIXED_SYSTEM_PATH = ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin"]

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

def _command_output(repository_ctx, command, environment, description):
    result = repository_ctx.execute(
        command,
        environment = environment,
        quiet = True,
    )
    if result.return_code:
        detail = result.stderr.strip() or result.stdout.strip() or "no output"
        fail("cannot validate %s: %s" % (description, detail))
    return result.stdout.strip()

def _key_value_contract(repository_ctx, label, description):
    entries = {}
    for raw_line in repository_ctx.read(label).splitlines():
        line = raw_line.strip()
        if not line:
            continue
        key, separator, value = line.partition("=")
        if not separator or not key or not value or key in entries:
            fail("invalid %s entry: %s" % (description, raw_line))
        entries[key] = value
    return entries

def _family_executable(tools_root, family, executable):
    family_root = tools_root.get_child("tools").get_child(family)
    matches = []
    if family_root.exists:
        for version in family_root.readdir():
            candidate = version.get_child(family).get_child("bin").get_child(executable)
            if candidate.exists:
                matches.append(candidate)
    if len(matches) != 1:
        fail("expected exactly one %s under %s, found %d" % (
            executable,
            family_root,
            len(matches),
        ))
    return matches[0]

def _system_executable(repository_ctx, name):
    matches = []
    for directory in _FIXED_SYSTEM_PATH:
        candidate = repository_ctx.path(directory).get_child(name)
        if candidate.exists:
            matches.append(candidate)
    if not matches:
        fail("%s is unavailable in the fixed system path" % name)
    return matches[0]

def _watch_git_state(repository_ctx, root):
    repository_ctx.watch_tree(root)
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
        "IDF_PATH",
        "BK7258_PATH",
        "BK3633_PATH",
        "JIELI_AC695N_SDK_PATH",
        "JIELI_AC791N_SDK_PATH",
    ],
)

def _esp_tools_repository_impl(repository_ctx):
    tools_root_value = _environment_value(repository_ctx, "IDF_TOOLS_PATH")
    if not tools_root_value:
        _write_locator(
            repository_ctx,
            "esp-idf-tools",
            False,
            metadata = {"reason": "IDF_TOOLS_PATH is unset"},
        )
        return
    tools_root = repository_ctx.path(tools_root_value)
    if not tools_root.get_child("tools").exists:
        fail("ESP-IDF tools directory is unavailable: %s" % tools_root)
    sdk_locator = json.decode(repository_ctx.read(repository_ctx.attr.sdk_locator))
    if not sdk_locator.get("enabled", False):
        fail("ESP-IDF SDK locator must be configured before ESP tools validation")
    idf_root_value = sdk_locator.get("paths", {}).get("root", "")
    if not idf_root_value:
        fail("ESP-IDF SDK locator has no root")
    idf_root = repository_ctx.path(idf_root_value)
    contract = _key_value_contract(
        repository_ctx,
        repository_ctx.attr.tool_versions_file,
        "ESP-IDF tool version",
    )
    required = ["esp32p4", "esp32s3", "ninja", "python"]
    missing = [key for key in required if not contract.get(key)]
    if missing:
        fail("ESP-IDF tool versions are incomplete: %s" % ", ".join(missing))
    if contract["python"] != "esp-idf-v6.0-constraints":
        fail("unsupported ESP-IDF Python identity: %s" % contract["python"])
    python_environment_root = tools_root.get_child("python_env")
    if not python_environment_root.exists or not python_environment_root.is_dir:
        fail("ESP-IDF Python environment directory is unavailable: %s" % python_environment_root)
    python_environment_names = sorted([
        candidate.basename
        for candidate in python_environment_root.readdir()
        if candidate.is_dir and
           candidate.basename.startswith("idf6.0_py") and
           candidate.basename.endswith("_env") and
           candidate.get_child("bin").get_child("python").exists
    ])
    if len(python_environment_names) != 1:
        fail("expected exactly one ESP-IDF 6.0 Python environment under %s, found %s" % (
            python_environment_root,
            python_environment_names,
        ))
    python_root = python_environment_root.get_child(python_environment_names[0])
    python = python_root.get_child("bin").get_child("python")
    compiler_contracts = [
        ("esp32p4", "riscv32-esp-elf", "riscv32-esp-elf-gcc"),
        ("esp32s3", "xtensa-esp-elf", "xtensa-esp-elf-gcc"),
    ]
    environment = {
        "HOME": "/tmp",
        "IDF_PATH": str(idf_root.realpath),
        "IDF_PYTHON_ENV_PATH": str(python_root.realpath),
        "IDF_TOOLS_PATH": str(tools_root.realpath),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "PATH": ":".join(_FIXED_SYSTEM_PATH),
        "PYTHONDONTWRITEBYTECODE": "1",
        "TZ": "UTC",
    }
    for key, family, executable in compiler_contracts:
        compiler = _family_executable(tools_root, family, executable)
        actual = _command_output(
            repository_ctx,
            [str(compiler), "--version"],
            environment,
            "%s compiler" % key,
        ).splitlines()[0]
        if actual != contract[key]:
            fail("%s compiler version mismatch: expected %r, found %r" % (
                key,
                contract[key],
                actual,
            ))
    ninja = _system_executable(repository_ctx, "ninja")
    ninja_version = _command_output(
        repository_ctx,
        [str(ninja), "--version"],
        environment,
        "Ninja",
    )
    if ninja_version != contract["ninja"]:
        fail("Ninja version mismatch: expected %r, found %r" % (
            contract["ninja"],
            ninja_version,
        ))
    idf_tools = idf_root.get_child("tools").get_child("idf_tools.py")
    for operation in ["check", "check-python-dependencies"]:
        _command_output(
            repository_ctx,
            [str(python), str(idf_tools), "--non-interactive", operation],
            environment,
            "ESP-IDF tools %s" % operation,
        )
    repository_ctx.watch_tree(tools_root)
    _write_locator(
        repository_ctx,
        "esp-idf-tools",
        True,
        paths = {
            "python_root": str(python_root.realpath),
            "tools_root": str(tools_root.realpath),
        },
    )

_esp_tools_repository = repository_rule(
    implementation = _esp_tools_repository_impl,
    attrs = {
        "sdk_locator": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "tool_versions_file": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
    },
    environ = ["IDF_TOOLS_PATH"],
)

def _ccache_runtime_repository_impl(repository_ctx):
    value = _environment_value(repository_ctx, "H2_NATIVE_CCACHE_RUNTIME_ROOT")
    if not value:
        _write_locator(
            repository_ctx,
            "native-ccache-runtime",
            False,
            metadata = {"reason": "environment variable is unset"},
        )
        return
    root = repository_ctx.path(value)
    manifest = root.get_child("runtime.json")
    if not root.exists or not root.is_dir:
        fail("H2_NATIVE_CCACHE_RUNTIME_ROOT is not a directory: %s" % root)
    if not manifest.exists:
        fail("native ccache runtime manifest is missing: %s" % manifest)
    _write_locator(
        repository_ctx,
        "native-ccache-runtime",
        True,
        paths = {
            "manifest": str(manifest.realpath),
            "root": str(root.realpath),
        },
    )

_ccache_runtime_repository = repository_rule(
    implementation = _ccache_runtime_repository_impl,
    environ = ["H2_NATIVE_CCACHE_RUNTIME_ROOT"],
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
filegroup(name = \"all_files\", srcs = glob([\"toolchain/**\"], allow_empty = False))
""",
    )

_arm_toolchain_repository = repository_rule(
    implementation = _arm_toolchain_repository_impl,
    attrs = {
        "archives_file": attr.label(allow_single_file = True, mandatory = True),
        "identity_tool": attr.label(allow_single_file = True, mandatory = True),
    },
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
        name = "h2_esp_idf_sdk",
        commit_file = "//tools/bazel:native_versions/esp_idf_commit.txt",
        environment_variable = "IDF_PATH",
        kind = "esp-idf-sdk",
        required_files = ["tools/idf.py", "tools/cmake/version.cmake"],
    )
    _esp_tools_repository(
        name = "h2_esp_idf_tools",
        sdk_locator = "@h2_esp_idf_sdk//:locator.json",
        tool_versions_file = "//tools/bazel:native_versions/esp_idf_tool_versions.txt",
    )
    _local_sdk_repository(
        name = "h2_bk7258_sdk",
        commit_file = "//tools/bazel:native_versions/bk7258_sdk_commit.txt",
        environment_variable = "BK7258_PATH",
        kind = "bk7258-sdk",
        required_files = ["Makefile"],
    )
    _local_sdk_repository(
        name = "h2_bk3633_sdk",
        commit_file = "//tools/bazel:native_versions/bk3633_sdk_commit.txt",
        environment_variable = "BK3633_PATH",
        kind = "bk3633-sdk",
        required_files = [
            "SDK/src/system/BK3633_STACK_ALLROLES.elf",
            "SDK/projects/app_gatt_all_roles/output/stack/BK3633_STACK_ALLROLES.bin",
            "SDK/projects/boot_for_all_roles/Makefile",
        ],
    )
    _arm_toolchain_repository(
        name = "h2_bk_arm_toolchain",
        archives_file = "//tools/bazel:native_versions/bk_toolchain_archives.txt",
        identity_tool = "//tools/bazel:toolchain_identity.py",
    )
    _local_sdk_repository(
        name = "h2_jieli_ac695n_sdk",
        # firmware-devenv exports the SDK root (jieli_ac695n_sdk/SDK) inside
        # the mirror checkout.
        allow_subdirectory = True,
        commit_file = "//tools/bazel:native_versions/jieli_ac695n_sdk_commit.txt",
        environment_variable = "JIELI_AC695N_SDK_PATH",
        kind = "jieli-ac695n-sdk",
        required_files = [
            "Makefile",
            "cpu/br23/tools/download.c",
            "cpu/br23/tools/download/standard/download.bat",
        ],
    )
    _local_sdk_repository(
        name = "h2_jieli_ac791n_sdk",
        allow_subdirectory = True,
        commit_file = "//tools/bazel:native_versions/jieli_ac791n_sdk_commit.txt",
        environment_variable = "JIELI_AC791N_SDK_PATH",
        kind = "jieli-ac791n-sdk",
        required_files = [
            "Makefile",
            "cpu/wl82/tools/download.c",
            "apps/demo/demo_hello/board/wl82/Makefile",
        ],
    )
    _jieli_tools_repository(
        name = "h2_jieli_toolchain",
        archives_file = "//tools/bazel:native_versions/jieli_toolchain_archives.txt",
        contract_kind = "toolchain",
        environment_variable = "JIELI_TOOLCHAIN_ROOT",
        exported_paths = {
            "pi32v2_bin": "pi32v2/bin",
            "q32s_bin": "q32s/bin",
        },
        identity_tool = "//tools/bazel:toolchain_identity.py",
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
        archives_file = "//tools/bazel:native_versions/jieli_toolchain_archives.txt",
        contract_kind = "postbuild",
        environment_variable = "JIELI_POSTBUILD_ROOT",
        identity_tool = "//tools/bazel:toolchain_identity.py",
        kind = "jieli-postbuild",
        required_files = [
            "fw_add",
            "isd_download",
            "pack_res",
            "remove_tailing_zeros",
            "ufw_maker",
        ],
    )
    _ccache_runtime_repository(name = "h2_native_ccache_runtime")

native_repositories = module_extension(implementation = _extension_impl)
