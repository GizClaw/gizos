"""C/C++ toolchain configuration for Arm GNU 8.2 arm-linux-gnueabihf."""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:cc_toolchain_config_lib.bzl", "feature", "flag_group", "flag_set", "tool_path")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl", "CcToolchainConfigInfo")

_COMPILE_ACTIONS = [
    ACTION_NAMES.c_compile,
    ACTION_NAMES.cpp_compile,
    ACTION_NAMES.linkstamp_compile,
    ACTION_NAMES.assemble,
    ACTION_NAMES.preprocess_assemble,
]
_LINK_ACTIONS = [
    ACTION_NAMES.cpp_link_executable,
    ACTION_NAMES.cpp_link_dynamic_library,
    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
]

def _config_impl(ctx):
    deterministic = feature(
        name = "k4b_deterministic",
        enabled = True,
        flag_sets = [flag_set(
            actions = _COMPILE_ACTIONS,
            flag_groups = [flag_group(flags = ctx.attr.compile_flags + [
                "-no-canonical-prefixes",
                "-fno-canonical-system-headers",
                "-Wno-builtin-macro-redefined",
                "-D__DATE__=\"redacted\"",
                "-D__TIME__=\"redacted\"",
                "-D__TIMESTAMP__=\"redacted\"",
            ])],
        )],
    )
    linker = feature(
        name = "k4b_linker",
        enabled = True,
        flag_sets = [flag_set(
            actions = _LINK_ACTIONS,
            flag_groups = [flag_group(flags = ["-Wl,--build-id=sha1"])],
        )],
    )
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        features = [deterministic, linker],
        cxx_builtin_include_directories = ctx.attr.builtin_include_directories,
        toolchain_identifier = "arm-gnu-8.2.1-arm-linux-gnueabihf",
        host_system_name = "x86_64-linux-gnu",
        target_system_name = "arm-linux-gnueabihf",
        target_cpu = "armv7",
        target_libc = "glibc-2.28",
        compiler = "gcc-8.2.1",
        abi_version = "eabi5",
        abi_libc_version = "glibc-2.28",
        tool_paths = [tool_path(name = name, path = path) for name, path in ctx.attr.tool_paths.items()],
    )

k4b_cc_toolchain_config = rule(
    implementation = _config_impl,
    attrs = {
        "builtin_include_directories": attr.string_list(),
        "compile_flags": attr.string_list(),
        "tool_paths": attr.string_dict(),
    },
    provides = [CcToolchainConfigInfo],
)
