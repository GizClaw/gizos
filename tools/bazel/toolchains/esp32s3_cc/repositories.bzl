"""ESP32-S3 C/C++ toolchain materialized from native repositories."""

_TOOLS = {
    "ar": "xtensa-esp-elf-ar",
    "cpp": "xtensa-esp-elf-cpp",
    "gcc": "xtensa-esp-elf-gcc",
    "gcov": "xtensa-esp-elf-gcov",
    "ld": "xtensa-esp-elf-ld",
    "nm": "xtensa-esp-elf-nm",
    "objcopy": "xtensa-esp-elf-objcopy",
    "objdump": "xtensa-esp-elf-objdump",
    "strip": "xtensa-esp-elf-strip",
}

def _tool_version_contract(repository_ctx):
    entries = {}
    for line in repository_ctx.read(repository_ctx.attr._versions_file).splitlines():
        key, separator, value = line.partition("=")
        if separator and key and value:
            entries[key] = value
    return entries

def _mirror_file(repository_ctx, source, destination):
    if not source.exists:
        fail("required ESP32-S3 toolchain input is missing: %s" % source)
    repository_ctx.symlink(source, destination)

def _mirror_tree(repository_ctx, source_root, destination_root):
    discovered = repository_ctx.execute([
        "find",
        "-L",
        str(source_root),
        "-type",
        "f",
    ], quiet = True)
    if discovered.return_code:
        fail("failed to enumerate ESP32-S3 toolchain inputs under %s: %s" % (
            source_root,
            discovered.stderr,
        ))
    prefix = str(source_root) + "/"
    for source in discovered.stdout.splitlines():
        if source.startswith(prefix):
            repository_ctx.symlink(
                source,
                destination_root + "/" + source[len(prefix):],
            )

def _expected_compiler_contract(repository_ctx):
    contract = _tool_version_contract(repository_ctx)
    expected_driver = contract.get("esp32s3_archive_compiler")
    if expected_driver != _TOOLS["gcc"]:
        fail("ESP32-S3 archive compiler mismatch: expected %r, configured %r" % (
            expected_driver,
            _TOOLS["gcc"],
        ))
    expected_version = contract.get("esp32s3")
    if not expected_version:
        fail("ESP tool-version contract has no esp32s3 entry")
    expected_dynconfig = contract.get("esp32s3_dynconfig")
    if expected_dynconfig != "xtensa_esp32s3.so":
        fail("ESP32-S3 dynamic configuration mismatch: %r" % expected_dynconfig)
    return expected_version, expected_dynconfig

def _validate_compiler_version(repository_ctx, compiler):
    result = repository_ctx.execute([compiler, "--version"], quiet = True)
    if result.return_code:
        fail("failed to inspect ESP32-S3 compiler version: %s" % result.stderr)
    actual = result.stdout.splitlines()[0] if result.stdout else ""
    expected, _ = _expected_compiler_contract(repository_ctx)
    if actual != expected:
        fail("ESP32-S3 compiler version mismatch: expected %r, got %r" % (
            expected,
            actual,
        ))

def _toolchain_root(repository_ctx):
    locator = json.decode(repository_ctx.read(repository_ctx.attr.tools_locator))
    if not locator.get("enabled", False):
        return None
    tools_path = locator.get("paths", {}).get("tools_root", "")
    if not tools_path:
        fail("ESP-IDF tools locator has no tools_root")
    family = repository_ctx.path(tools_path).get_child("tools").get_child("xtensa-esp-elf")
    if not family.exists:
        return None
    matches = []
    for version in family.readdir():
        candidate = version.get_child("xtensa-esp-elf")
        if candidate.get_child("bin").get_child(_TOOLS["gcc"]).exists:
            matches.append(candidate)
    if len(matches) != 1:
        fail("expected exactly one ESP Xtensa toolchain under %s, found %d" % (
            family,
            len(matches),
        ))
    return matches[0]

def _wrapper(tool, use_esp32s3_dynconfig = False):
    configure_target = ""
    if use_esp32s3_dynconfig:
        configure_target = """
toolchain_root=${selected%%/bin/%s}
dynamic_config=${toolchain_root}/lib/xtensa_esp32s3.so
if [ ! -f "${dynamic_config}" ]; then
    echo "error: ESP32-S3 Xtensa configuration is unavailable: ${dynamic_config}" >&2
    exit 1
fi
export XTENSA_GNU_CONFIG="${dynamic_config}"
""" % tool
    return """#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
selected="${root}/inputs/compiler/bin/%s"
%s
exec "${selected}" "$@"
""" % (tool, configure_target)

def _dynamic_config(repository_ctx, root):
    if not root:
        return None
    _, dynamic_config_name = _expected_compiler_contract(repository_ctx)
    dynamic_config = root.get_child("lib").get_child(dynamic_config_name)
    if not dynamic_config.exists:
        fail("ESP32-S3 Xtensa configuration is unavailable: %s" % dynamic_config)
    return dynamic_config

def _builtin_include_directories(repository_ctx, root, dynamic_config):
    if not root:
        return []
    compiler = root.get_child("bin").get_child(_TOOLS["gcc"])
    result = repository_ctx.execute([
        compiler,
        "-E",
        "-x",
        "c",
        "/dev/null",
        "-v",
    ], environment = {
        "XTENSA_GNU_CONFIG": str(dynamic_config),
    })
    if result.return_code:
        fail("failed to inspect ESP32-S3 compiler includes:\n%s" % result.stderr)
    directories = []
    recording = False
    for raw_line in result.stderr.splitlines():
        line = raw_line.strip()
        if line == "#include <...> search starts here:":
            recording = True
        elif line == "End of search list.":
            break
        elif recording and line and not line.startswith("("):
            directories.append(line)
    if not directories:
        fail("ESP32-S3 compiler reported no builtin include directories")
    return directories

def _idf_builtin_include_directories(repository_ctx):
    locator = json.decode(repository_ctx.read(repository_ctx.attr.sdk_locator))
    if not locator.get("enabled", False):
        return []
    idf_path = locator.get("paths", {}).get("root", "")
    if not idf_path:
        fail("ESP-IDF SDK locator has no root")
    xtensa_root = repository_ctx.path(idf_path).get_child("components").get_child("xtensa")
    include_root = xtensa_root.get_child("include")
    target_include_root = xtensa_root.get_child("esp32s3").get_child("include")
    if not include_root.get_child("xtensa").get_child("coreasm.h").exists:
        fail("ESP-IDF Xtensa system headers are unavailable under %s" % include_root)
    if not target_include_root.get_child("xtensa").get_child("config").get_child("core.h").exists:
        fail("ESP32-S3 Xtensa config headers are unavailable under %s" % target_include_root)
    return [
        str(include_root),
        str(target_include_root),
    ]

def _declare_action_inputs(repository_ctx, root, dynamic_config, builtin_include_directories, idf_include_directories):
    if not root:
        return
    compiler = root.get_child("bin").get_child(_TOOLS["gcc"])
    _validate_compiler_version(repository_ctx, compiler)
    _mirror_file(repository_ctx, compiler, "inputs/compiler/bin/gcc")
    _mirror_file(
        repository_ctx,
        dynamic_config,
        "inputs/compiler/lib/xtensa_esp32s3.so",
    )
    _mirror_file(
        repository_ctx,
        root.get_child("bin").get_child("xtensa-esp-elf-as"),
        "inputs/compiler/bin/as",
    )
    cc1 = repository_ctx.execute([compiler, "-print-prog-name=cc1"], quiet = True)
    if cc1.return_code or not cc1.stdout.strip():
        fail("failed to resolve the ESP32-S3 cc1 executable: %s" % cc1.stderr)
    _mirror_file(
        repository_ctx,
        repository_ctx.path(cc1.stdout.strip()),
        "inputs/compiler/libexec/cc1",
    )
    _mirror_file(
        repository_ctx,
        root.get_child("bin").get_child(_TOOLS["ar"]),
        "inputs/archiver/bin/ar",
    )
    for index, directory in enumerate(builtin_include_directories):
        _mirror_tree(
            repository_ctx,
            repository_ctx.path(directory),
            "inputs/compiler/include/%d" % index,
        )
    for index, directory in enumerate(idf_include_directories):
        _mirror_tree(
            repository_ctx,
            repository_ctx.path(directory),
            "inputs/sdk/include/%d" % index,
        )

def _esp32s3_cc_repository_impl(repository_ctx):
    exec_constraints = _exec_constraints(repository_ctx)
    if not exec_constraints:
        repository_ctx.file(
            "BUILD.bazel",
            '''package(default_visibility = ["//visibility:public"])

filegroup(name = "unavailable")

toolchain(
    name = "toolchain",
    target_compatible_with = ["@platforms//:incompatible"],
    toolchain = ":unavailable",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
''',
        )
        return
    root = _toolchain_root(repository_ctx)
    if not root:
        repository_ctx.file(
            "BUILD.bazel",
            '''package(default_visibility = ["//visibility:public"])

filegroup(name = "unavailable")

toolchain(
    name = "toolchain",
    target_compatible_with = ["@platforms//:incompatible"],
    toolchain = ":unavailable",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
''',
        )
        return
    dynamic_config = _dynamic_config(repository_ctx, root)
    builtin_include_directories = _builtin_include_directories(
        repository_ctx,
        root,
        dynamic_config,
    )
    idf_include_directories = _idf_builtin_include_directories(repository_ctx)
    _declare_action_inputs(
        repository_ctx,
        root,
        dynamic_config,
        builtin_include_directories,
        idf_include_directories,
    )
    for name, executable in _TOOLS.items():
        _mirror_file(
            repository_ctx,
            root.get_child("bin").get_child(executable),
            "inputs/compiler/bin/" + executable,
        )
        repository_ctx.file(
            "bin/" + name,
            _wrapper(executable, use_esp32s3_dynconfig = name == "gcc"),
            executable = True,
        )
    repository_ctx.template(
        "BUILD.bazel",
        repository_ctx.attr._build_file,
        substitutions = {
            "{BUILTIN_INCLUDE_DIRECTORIES}": repr(
                builtin_include_directories +
                idf_include_directories,
            ),
            "{EXEC_CONSTRAINTS}": repr(exec_constraints),
            "{SYSTEM_INCLUDE_DIRECTORIES}": repr(idf_include_directories),
        },
    )
    repository_ctx.file(
        "cc_toolchain_config.bzl",
        repository_ctx.read(repository_ctx.attr._config_file),
    )

def _exec_constraints(repository_ctx):
    name = repository_ctx.os.name.lower()
    arch = repository_ctx.os.arch.lower()
    if name in ("mac os x", "macos") and arch in ("aarch64", "arm64"):
        return ["@platforms//cpu:arm64", "@platforms//os:macos"]
    if name == "linux" and arch in ("amd64", "x86_64"):
        return ["@platforms//cpu:x86_64", "@platforms//os:linux"]
    return None

_esp32s3_cc_repository = repository_rule(
    implementation = _esp32s3_cc_repository_impl,
    attrs = {
        "_build_file": attr.label(
            default = "//tools/bazel/toolchains/esp32s3_cc:toolchain.BUILD.bazel",
        ),
        "_config_file": attr.label(
            default = "//tools/bazel/toolchains/esp32s3_cc:cc_toolchain_config.bzl",
        ),
        "_versions_file": attr.label(
            allow_single_file = True,
            default = "//tools/bazel:native_versions/esp_idf_tool_versions.txt",
        ),
        "sdk_locator": attr.label(
            allow_single_file = True,
            default = "@gizos_esp_idf_sdk//:locator.json",
        ),
        "tools_locator": attr.label(
            allow_single_file = True,
            default = "@gizos_esp_idf_tools//:locator.json",
        ),
    },
)

def _esp32s3_cc_extension_impl(module_ctx):
    configurations = []
    for module in module_ctx.modules:
        configurations.extend(module.tags.locators)
    if len(configurations) > 1:
        fail("esp32s3_cc_toolchain accepts at most one locator configuration")
    if configurations:
        _esp32s3_cc_repository(
            name = "gizos_esp32s3_cc_toolchain",
            sdk_locator = configurations[0].sdk_locator,
            tools_locator = configurations[0].tools_locator,
        )
    else:
        _esp32s3_cc_repository(name = "gizos_esp32s3_cc_toolchain")

esp32s3_cc_toolchain = module_extension(
    implementation = _esp32s3_cc_extension_impl,
    tag_classes = {
        "locators": tag_class(attrs = {
            "sdk_locator": attr.label(allow_single_file = True, mandatory = True),
            "tools_locator": attr.label(allow_single_file = True, mandatory = True),
        }),
    },
)
