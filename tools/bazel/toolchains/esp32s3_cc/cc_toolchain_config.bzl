"""C/C++ toolchain configuration for ESP32-S3 portable archives."""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:cc_toolchain_config_lib.bzl", "feature", "flag_group", "flag_set", "tool_path")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl", "CcToolchainConfigInfo")

_C_COMPILE_ACTIONS = [
    ACTION_NAMES.assemble,
    ACTION_NAMES.c_compile,
    ACTION_NAMES.preprocess_assemble,
]

_CXX_COMPILE_ACTIONS = [ACTION_NAMES.cpp_compile, ACTION_NAMES.linkstamp_compile]

_COMPILE_ACTIONS = _C_COMPILE_ACTIONS + _CXX_COMPILE_ACTIONS

def _include_flags(ctx, compiler_directories):
    flags = ["-nostdinc"]
    for option, directories in [
        ("-isystem", ctx.attr.system_include_directories),
        ("-idirafter", compiler_directories),
    ]:
        for directory in directories:
            path = "/".join([part for part in [ctx.label.workspace_root, ctx.label.package, directory] if part])
            flags.extend([option, path])
    return flags

def _config_impl(ctx):
    compile_flags = feature(
        name = "h2_esp32s3_compile_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = _COMPILE_ACTIONS,
            flag_groups = [flag_group(flags = [
                "-Os",
                "-mlongcalls",
                "-fno-builtin-memcpy",
                "-fno-builtin-memset",
                "-fstrict-volatile-bitfields",
                "-ffunction-sections",
                "-fdata-sections",
                "-fno-common",
                "-fno-ident",
                "-fno-canonical-system-headers",
                "-fno-record-gcc-switches",
                "-Wno-builtin-macro-redefined",
                "-D__DATE__=\"redacted\"",
                "-D__TIME__=\"redacted\"",
                "-D__TIMESTAMP__=\"redacted\"",
            ])],
        )],
    )
    system_includes = feature(
        name = "h2_esp32s3_system_includes",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _C_COMPILE_ACTIONS,
                flag_groups = [flag_group(flags = _include_flags(ctx, ctx.attr.c_include_directories))],
            ),
            flag_set(
                actions = _CXX_COMPILE_ACTIONS,
                flag_groups = [flag_group(flags = _include_flags(ctx, ctx.attr.cxx_include_directories))],
            ),
        ],
    )
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        abi_libc_version = "newlib",
        abi_version = "elf",
        compiler = "xtensa-esp-elf-gcc",
        cxx_builtin_include_directories = ["%crosstool_top%/" + directory for directory in ctx.attr.builtin_include_directories],
        features = [compile_flags, system_includes],
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
        toolchain_identifier = "esp32s3-local-esp-16.1.0",
    )

esp32s3_cc_toolchain_config = rule(
    implementation = _config_impl,
    attrs = {
        "builtin_include_directories": attr.string_list(),
        "c_include_directories": attr.string_list(),
        "cxx_include_directories": attr.string_list(),
        "system_include_directories": attr.string_list(),
    },
    provides = [CcToolchainConfigInfo],
)
