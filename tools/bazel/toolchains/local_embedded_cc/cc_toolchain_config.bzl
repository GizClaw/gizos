"""C/C++ toolchain configuration for repository-selected embedded compilers."""

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
    compile_flags = feature(
        name = "h2_embedded_compile_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = _COMPILE_ACTIONS,
            flag_groups = [flag_group(flags = ctx.attr.compile_flags)],
        )],
    )
    features = [compile_flags]
    if ctx.attr.unfiltered_compile_flags:
        # Bazel emits features in declaration order and only appends the legacy
        # `user_compile_flags`/`unfiltered_compile_flags` features when the
        # config does not define them. Declaring both here, in this order,
        # places the toolchain overrides after every per-target copt so they
        # win over `-Wextra`/`-Werror` that targets and vendor overlays add.
        features.append(feature(
            name = "user_compile_flags",
            enabled = True,
            flag_sets = [flag_set(
                actions = _COMPILE_ACTIONS,
                flag_groups = [flag_group(
                    flags = ["%{user_compile_flags}"],
                    iterate_over = "user_compile_flags",
                    expand_if_available = "user_compile_flags",
                )],
            )],
        ))
        features.append(feature(
            name = "unfiltered_compile_flags",
            enabled = True,
            flag_sets = [flag_set(
                actions = _COMPILE_ACTIONS,
                flag_groups = [flag_group(flags = ctx.attr.unfiltered_compile_flags)],
            )],
        ))
    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        abi_libc_version = "newlib",
        abi_version = "elf",
        compiler = ctx.attr.compiler,
        cxx_builtin_include_directories = ctx.attr.builtin_include_directories,
        features = features,
        host_system_name = "local",
        target_cpu = ctx.attr.target_cpu,
        target_libc = "newlib",
        target_system_name = ctx.attr.target_system_name,
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
        toolchain_identifier = ctx.attr.toolchain_identifier,
    )

local_embedded_cc_toolchain_config = rule(
    implementation = _config_impl,
    attrs = {
        "builtin_include_directories": attr.string_list(),
        "compile_flags": attr.string_list(),
        "compiler": attr.string(mandatory = True),
        "target_cpu": attr.string(mandatory = True),
        "target_system_name": attr.string(mandatory = True),
        "toolchain_identifier": attr.string(mandatory = True),
        "unfiltered_compile_flags": attr.string_list(),
    },
    provides = [CcToolchainConfigInfo],
)
