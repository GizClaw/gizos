"""Embedded C/C++ toolchains materialized from native dependency repositories."""

_TOOLS = ["ar", "as", "cpp", "gcc", "gcov", "ld", "nm", "objcopy", "objdump", "strip"]

def _contract(repository_ctx):
    entries = {}
    for line in repository_ctx.read(repository_ctx.attr.versions_file).splitlines():
        key, separator, value = line.partition("=")
        if separator and key and value:
            entries[key] = value
    return entries

def _exec_host(repository_ctx):
    name = repository_ctx.os.name.lower()
    arch = repository_ctx.os.arch.lower()
    if name in ("mac os x", "macos") and arch in ("aarch64", "arm64"):
        return "macos_arm64"
    if name == "linux" and arch in ("amd64", "x86_64"):
        return "linux_x86_64"
    fail("unsupported local embedded toolchain execution platform: %s/%s" % (name, arch))

def _exec_constraints(repository_ctx):
    return {
        "linux_x86_64": ["@platforms//cpu:x86_64", "@platforms//os:linux"],
        "macos_arm64": ["@platforms//cpu:arm64", "@platforms//os:macos"],
    }[_exec_host(repository_ctx)]

def _tool_names(repository_ctx):
    """Map Bazel tool names to executables in the selected bin directory.

    GCC layouts use `<prefix><tool>`; clang layouts override the compiler,
    preprocessor, assembler and linker entries through `tool_overrides`.
    """
    names = {}
    for name in _TOOLS:
        names[name] = repository_ctx.attr.tool_overrides.get(name, repository_ctx.attr.prefix + name)
    return names

def _toolchain_bin(repository_ctx):
    locator = json.decode(repository_ctx.read(repository_ctx.attr.locator))
    if not locator.get("enabled", False):
        return None
    paths = locator.get("paths", {})
    environment_root = paths.get(repository_ctx.attr.locator_path, "")
    if not environment_root:
        fail("%s locator has no %s path" % (repository_ctx.attr.name, repository_ctx.attr.locator_path))
    root = repository_ctx.path(environment_root)
    if repository_ctx.attr.layout == "bin_dir":
        return root
    family = root.get_child("tools").get_child(repository_ctx.attr.family)
    matches = []
    if family.exists:
        for version in family.readdir():
            candidate = version.get_child(repository_ctx.attr.family).get_child("bin")
            if candidate.get_child(repository_ctx.attr.prefix + "gcc").exists:
                matches.append(candidate)
    if len(matches) != 1:
        fail("expected exactly one %s toolchain under %s, found %d" % (
            repository_ctx.attr.name,
            family,
            len(matches),
        ))
    return matches[0]

def _mirror_file(repository_ctx, source, destination):
    if not source.exists:
        fail("required %s toolchain input is missing: %s" % (repository_ctx.attr.name, source))
    repository_ctx.symlink(source, destination)

def _mirror_tree(repository_ctx, source_root, destination_root):
    discovered = repository_ctx.execute(["find", "-L", str(source_root), "-type", "f"], quiet = True)
    if discovered.return_code:
        fail("failed to enumerate toolchain inputs under %s: %s" % (source_root, discovered.stderr))
    prefix = str(source_root) + "/"
    for source in discovered.stdout.splitlines():
        if source.startswith(prefix):
            repository_ctx.symlink(source, destination_root + "/" + source[len(prefix):])

def _builtin_includes(repository_ctx, compiler, compile_flags):
    result = repository_ctx.execute([compiler, "-E", "-x", "c", "/dev/null", "-v"] + compile_flags)
    if result.return_code:
        fail("failed to inspect %s compiler includes:\n%s" % (repository_ctx.attr.name, result.stderr))
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
        fail("%s compiler reported no builtin include directories" % repository_ctx.attr.name)
    return directories

def _wrapper(repository_ctx, executable):
    return """#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
selected="${root}/inputs/toolchain/bin/%s"
[ -x "${selected}" ] || { echo "error: missing tool: ${selected}" >&2; exit 1; }
exec "${selected}" "$@"
""" % (
        executable,
    )

def _unavailable(repository_ctx):
    repository_ctx.file(
        "BUILD.bazel",
        """package(default_visibility = [\"//visibility:public\"])

filegroup(name = \"unavailable\")

toolchain(
    name = \"toolchain\",
    target_compatible_with = [\"@platforms//:incompatible\"],
    toolchain = \":unavailable\",
    toolchain_type = \"@bazel_tools//tools/cpp:toolchain_type\",
)
""",
    )

def _repository_impl(repository_ctx):
    bin_dir = _toolchain_bin(repository_ctx)
    if not bin_dir:
        _unavailable(repository_ctx)
        return
    if repository_ctx.attr.exec_hosts and _exec_host(repository_ctx) not in repository_ctx.attr.exec_hosts:
        # The selected compiler only ships binaries for other execution hosts
        # (for example the JieLi clang is Linux x86_64 only); register an
        # incompatible toolchain instead of failing repository evaluation.
        _unavailable(repository_ctx)
        return
    tool_names = _tool_names(repository_ctx)
    compiler = bin_dir.get_child(tool_names["gcc"])
    version = repository_ctx.execute([compiler, "--version"], quiet = True)
    actual_version = version.stdout.splitlines()[0] if version.stdout else ""
    expected_version = _contract(repository_ctx).get(repository_ctx.attr.version_key)
    if version.return_code or actual_version != expected_version:
        fail("%s compiler version mismatch: expected %r, got %r" % (
            repository_ctx.attr.name,
            expected_version,
            actual_version,
        ))
    compile_flags = list(repository_ctx.attr.compile_flags)
    for relative in repository_ctx.attr.system_include_dirs:
        directory = bin_dir.get_child(relative)
        if not directory.exists:
            fail("%s system include directory is missing: %s" % (repository_ctx.attr.name, directory))
        compile_flags.extend(["-isystem", str(directory.realpath)])
    builtin_includes = _builtin_includes(repository_ctx, compiler, compile_flags)
    _mirror_file(repository_ctx, compiler, "inputs/compiler/bin/gcc")
    if repository_ctx.attr.compiler_kind == "gcc":
        _mirror_file(repository_ctx, bin_dir.get_child(tool_names["as"]), "inputs/compiler/bin/as")
        cc1 = repository_ctx.execute([compiler, "-print-prog-name=cc1"], quiet = True)
        if cc1.return_code or not cc1.stdout.strip():
            fail("failed to resolve %s cc1 executable" % repository_ctx.attr.name)
        _mirror_file(repository_ctx, repository_ctx.path(cc1.stdout.strip()), "inputs/compiler/libexec/cc1")
    _mirror_file(repository_ctx, bin_dir.get_child(tool_names["ar"]), "inputs/archiver/bin/ar")
    for index, directory in enumerate(builtin_includes):
        _mirror_tree(repository_ctx, repository_ctx.path(directory), "inputs/compiler/include/%d" % index)
    mirrored = {}
    for name in _TOOLS:
        executable = tool_names[name]
        if executable not in mirrored:
            _mirror_file(repository_ctx, bin_dir.get_child(executable), "inputs/toolchain/bin/" + executable)
            mirrored[executable] = True
        repository_ctx.file("bin/" + name, _wrapper(repository_ctx, executable), executable = True)
    repository_ctx.template(
        "BUILD.bazel",
        repository_ctx.attr.build_file,
        substitutions = {
            "{BUILTIN_INCLUDE_DIRECTORIES}": repr(builtin_includes),
            "{COMPILE_FLAGS}": repr(compile_flags),
            "{COMPILER}": tool_names["gcc"],
            "{EXEC_CONSTRAINTS}": repr(_exec_constraints(repository_ctx)),
            "{TARGET_CONSTRAINTS}": repr(repository_ctx.attr.target_constraints),
            "{TARGET_CPU}": repository_ctx.attr.target_cpu,
            "{TARGET_SYSTEM_NAME}": repository_ctx.attr.target_system_name,
            "{TOOLCHAIN_IDENTIFIER}": repository_ctx.attr.toolchain_identifier,
            "{UNFILTERED_COMPILE_FLAGS}": repr(repository_ctx.attr.unfiltered_compile_flags),
            "{VERSION_LABEL}": str(repository_ctx.attr.versions_file),
        },
    )
    repository_ctx.file("cc_toolchain_config.bzl", repository_ctx.read(repository_ctx.attr.config_file))

local_embedded_cc_repository = repository_rule(
    implementation = _repository_impl,
    attrs = {
        "build_file": attr.label(default = "//tools/bazel/toolchains/local_embedded_cc:toolchain.BUILD.bazel"),
        "compile_flags": attr.string_list(),
        "compiler_kind": attr.string(default = "gcc", values = ["gcc", "clang"]),
        "config_file": attr.label(default = "//tools/bazel/toolchains/local_embedded_cc:cc_toolchain_config.bzl"),
        "exec_hosts": attr.string_list(
            doc = "Execution hosts that can run the selected compiler; empty means every supported host.",
        ),
        "family": attr.string(),
        "layout": attr.string(mandatory = True, values = ["bin_dir", "idf_tools"]),
        "locator": attr.label(allow_single_file = True, mandatory = True),
        "locator_path": attr.string(mandatory = True),
        "prefix": attr.string(mandatory = True),
        "system_include_dirs": attr.string_list(
            doc = "Bin-directory-relative libc include roots passed as -isystem (compilers without a builtin sysroot).",
        ),
        "target_constraints": attr.string_list(mandatory = True),
        "target_cpu": attr.string(mandatory = True),
        "target_system_name": attr.string(mandatory = True),
        "toolchain_identifier": attr.string(mandatory = True),
        "unfiltered_compile_flags": attr.string_list(
            doc = "Flags appended after every target copt (Bazel's unfiltered_compile_flags feature).",
        ),
        "tool_overrides": attr.string_dict(
            doc = "Bazel tool name to executable name overrides for non-GCC layouts.",
        ),
        "version_key": attr.string(mandatory = True),
        "versions_file": attr.label(allow_single_file = True, mandatory = True),
    },
)

def _extension_impl(_module_ctx):
    local_embedded_cc_repository(
        name = "gizos_bk3633_cc_toolchain",
        compile_flags = [
            "-Os",
            "-mcpu=arm968e-s",
            "-march=armv5te",
            "-mthumb",
            "-mthumb-interwork",
            "-ffunction-sections",
            "-fdata-sections",
        ],
        layout = "bin_dir",
        locator = "@gizos_bk_arm_toolchain//:locator.json",
        locator_path = "bin",
        prefix = "arm-none-eabi-",
        target_constraints = [
            "@gizos//tools/bazel/platforms:cpu_armv5te",
            "@gizos//tools/bazel/platforms:target_bk3633",
            "@platforms//os:none",
        ],
        target_cpu = "armv5te",
        target_system_name = "bk3633-elf",
        toolchain_identifier = "bk3633-local-arm-10.3.1",
        version_key = "arm_gcc",
        versions_file = "//tools/bazel:native_versions/bk_tool_versions.txt",
    )

    local_embedded_cc_repository(
        name = "gizos_bk7258_cc_toolchain",
        compile_flags = [
            "-Os",
            "-mcpu=cortex-m33+nodsp",
            "-mfpu=fpv5-sp-d16",
            "-mfloat-abi=hard",
            "-mcmse",
            "-ffunction-sections",
            "-fdata-sections",
        ],
        layout = "bin_dir",
        locator = "@gizos_bk_arm_toolchain//:locator.json",
        locator_path = "bin",
        prefix = "arm-none-eabi-",
        target_constraints = [
            "@gizos//tools/bazel/platforms:cpu_arm_m",
            "@gizos//tools/bazel/platforms:target_bk7258",
            "@platforms//os:none",
        ],
        target_cpu = "armv8-m",
        target_system_name = "bk7258-elf",
        toolchain_identifier = "bk7258-local-arm-10.3.1",
        version_key = "arm_gcc",
        versions_file = "//tools/bazel:native_versions/bk_tool_versions.txt",
    )

    local_embedded_cc_repository(
        name = "gizos_esp32c5_cc_toolchain",
        compile_flags = [
            "-Os",
            "-march=rv32imac_zicsr_zifencei",
            "-mabi=ilp32",
            "-ffunction-sections",
            "-fdata-sections",
            "-fno-common",
        ],
        family = "riscv32-esp-elf",
        layout = "idf_tools",
        locator = "@gizos_esp_idf_tools//:locator.json",
        locator_path = "tools_root",
        prefix = "riscv32-esp-elf-",
        target_constraints = [
            "@gizos//tools/bazel/platforms:cpu_riscv32",
            "@gizos//tools/bazel/platforms:target_esp32c5",
            "@platforms//os:none",
        ],
        target_cpu = "riscv32",
        target_system_name = "esp32c5-elf",
        toolchain_identifier = "esp32c5-local-esp-15.2.0",
        version_key = "esp32c5",
        versions_file = "//tools/bazel:native_versions/esp_idf_tool_versions.txt",
    )

    local_embedded_cc_repository(
        name = "gizos_esp32p4_cc_toolchain",
        compile_flags = [
            "-Os",
            "-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc",
            "-mabi=ilp32f",
            "-ffunction-sections",
            "-fdata-sections",
            "-fno-common",
        ],
        family = "riscv32-esp-elf",
        layout = "idf_tools",
        locator = "@gizos_esp_idf_tools//:locator.json",
        locator_path = "tools_root",
        prefix = "riscv32-esp-elf-",
        target_constraints = [
            "@gizos//tools/bazel/platforms:cpu_riscv32",
            "@gizos//tools/bazel/platforms:target_esp32p4",
            "@platforms//os:none",
        ],
        target_cpu = "riscv32",
        target_system_name = "esp32p4-elf",
        toolchain_identifier = "esp32p4-local-esp-15.2.0",
        version_key = "esp32p4",
        versions_file = "//tools/bazel:native_versions/esp_idf_tool_versions.txt",
    )

    local_embedded_cc_repository(
        name = "gizos_jieli_pi32v2_cc_toolchain",
        compile_flags = [
            "-target",
            "pi32v2",
            "-mcpu=r3",
            "-integrated-as",
            "-Oz",
            "-mllvm",
            "-pi32v2-large-program=true",
            "-fno-common",
            "-fallow-pointer-null",
            "-fprefer-gnu-section",
            "-fms-extensions",
            "-Wno-shift-negative-value",
        ],
        compiler_kind = "clang",
        exec_hosts = ["linux_x86_64"],
        layout = "bin_dir",
        locator = "@h2_jieli_toolchain//:locator.json",
        locator_path = "pi32v2_bin",
        prefix = "",
        system_include_dirs = ["../include"],
        target_constraints = [
            "@gizos//tools/bazel/platforms:cpu_pi32v2",
            "@platforms//os:none",
        ],
        target_cpu = "pi32v2",
        target_system_name = "pi32v2-elf",
        tool_overrides = {
            "as": "clang",
            "cpp": "clang",
            "gcc": "clang",
            "gcov": "clang",
            "ld": "lto-wrapper",
        },
        toolchain_identifier = "jieli-local-clang-4.0.1",
        unfiltered_compile_flags = [
            "-Wno-missing-field-initializers",
            "-Wno-missing-braces",
        ],
        version_key = "jieli_clang",
        versions_file = "//tools/bazel:native_versions/jieli_tool_versions.txt",
    )

local_embedded_cc_toolchains = module_extension(implementation = _extension_impl)
