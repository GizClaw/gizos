"""C/C++ toolchain configuration for ESP32-S3 portable archives."""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:cc_toolchain_config_lib.bzl", "feature", "flag_group", "flag_set", "tool_path")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl", "CcToolchainConfigInfo")

_COMPILE_ACTIONS = [
    ACTION_NAMES.assemble,
    ACTION_NAMES.c_compile,
    ACTION_NAMES.cpp_compile,
    ACTION_NAMES.linkstamp_compile,
    ACTION_NAMES.preprocess_assemble,
]

def _config_impl(ctx):
    system_include_flags = [
        "-isystem" + directory
        for directory in ctx.attr.system_include_directories
    ]
    compile_flags = feature(
        name = "h2_esp32s3_compile_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = _COMPILE_ACTIONS,
            flag_groups = [flag_group(flags = system_include_flags + [
                "-Os",
                "-mlongcalls",
                "-fno-builtin-memcpy",
                "-fno-builtin-memset",
                "-fstrict-volatile-bitfields",
                "-ffunction-sections",
                "-fdata-sections",
                "-fno-common",
                "-fno-ident",
                "-fno-record-gcc-switches",
                "-Wno-builtin-macro-redefined",
                "-D__DATE__=\"redacted\"",
                "-D__TIME__=\"redacted\"",
                "-D__TIMESTAMP__=\"redacted\"",
            ])],
        )],
    )
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        abi_libc_version = "newlib",
        abi_version = "elf",
        compiler = "xtensa-esp-elf-gcc",
        cxx_builtin_include_directories = ctx.attr.builtin_include_directories,
        features = [compile_flags],
        host_system_name = "local",
        target_cpu = "xtensa",
        target_libc = "newlib",
        target_system_name = "esp32s3-elf",
        tool_paths = [
            tool_path(name = name, path = "bin/" + name)
            for name in [
                "ar",
                "cpp",
                "gcc",
                "gcov",
                "ld",
                "nm",
                "objcopy",
                "objdump",
                "strip",
            ]
        ],
        toolchain_identifier = "esp32s3-local-esp-15.2.0",
    )

esp32s3_cc_toolchain_config = rule(
    implementation = _config_impl,
    attrs = {
        "builtin_include_directories": attr.string_list(),
        "system_include_directories": attr.string_list(),
    },
    provides = [CcToolchainConfigInfo],
)
