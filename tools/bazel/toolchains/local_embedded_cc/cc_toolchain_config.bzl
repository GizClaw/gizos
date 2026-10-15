"""C/C++ toolchain configuration for repository-selected embedded compilers."""

load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:cc_toolchain_config_lib.bzl", "feature", "flag_group", "flag_set", "tool_path")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl", "CcToolchainConfigInfo")

_C_COMPILE_ACTIONS = [
    ACTION_NAMES.assemble,
    ACTION_NAMES.c_compile,
    ACTION_NAMES.preprocess_assemble,
]

_CXX_COMPILE_ACTIONS = [
    ACTION_NAMES.cpp_compile,
    ACTION_NAMES.linkstamp_compile,
]

_COMPILE_ACTIONS = _C_COMPILE_ACTIONS + _CXX_COMPILE_ACTIONS

def _system_include_flags(ctx, directories):
    # Every compiler search directory is mirrored inside this repository.
    # Naming the mirrors by execroot-relative path keeps dependency files free
    # of the output base, and -idirafter keeps them after every -I/-isystem,
    # where the compiler searched its own directories.
    flags = ["-nostdinc"]
    for directory in directories:
        path = "/".join([part for part in [ctx.label.workspace_root, ctx.label.package, directory] if part])
        flags.extend(["-idirafter", path])
    return flags

def _config_impl(ctx):
    compile_flags = feature(
        name = "h2_embedded_compile_flags",
        enabled = True,
        flag_sets = [flag_set(
            actions = _COMPILE_ACTIONS,
            flag_groups = [flag_group(flags = ctx.attr.compile_flags)],
        )],
    )
    system_includes = feature(
        name = "h2_embedded_system_includes",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = _C_COMPILE_ACTIONS,
                flag_groups = [flag_group(flags = _system_include_flags(ctx, ctx.attr.c_include_directories))],
            ),
            flag_set(
                actions = _CXX_COMPILE_ACTIONS,
                flag_groups = [flag_group(flags = _system_include_flags(ctx, ctx.attr.cxx_include_directories))],
            ),
        ],
    )
    features = [compile_flags, system_includes]
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
        cxx_builtin_include_directories = [
            "%crosstool_top%/" + directory
            for directory in ctx.attr.builtin_include_directories
        ],
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
        "builtin_include_directories": attr.string_list(
            doc = "Mirrored search directories, relative to the cc_toolchain package.",
        ),
        "c_include_directories": attr.string_list(
            doc = "Ordered C search directories, relative to the cc_toolchain package.",
        ),
        "compile_flags": attr.string_list(),
        "compiler": attr.string(mandatory = True),
        "cxx_include_directories": attr.string_list(
            doc = "Ordered C++ search directories, relative to the cc_toolchain package.",
        ),
        "target_cpu": attr.string(mandatory = True),
        "target_system_name": attr.string(mandatory = True),
        "toolchain_identifier": attr.string(mandatory = True),
        "unfiltered_compile_flags": attr.string_list(),
    },
    provides = [CcToolchainConfigInfo],
)
