"""Complete BK7258 firmware builds backed by canonical devenv."""

load(":firmware.bzl", "FirmwareVersionInfo")
load(":firmware_components.bzl", "collect_firmware_components", "firmware_components_aspect")
load(":native_component.bzl", "H2NativeComponentInfo", "collect_native_components")

Bk7258FirmwareInfo = provider(
    doc = "Structured native outputs from one BK7258 AP/CP firmware build.",
    fields = {
        "ap_elf": "AP application ELF.",
        "ap_image": "AP application binary image.",
        "ap_map": "AP application linker map.",
        "cp_elf": "CP application ELF.",
        "cp_image": "CP application binary image.",
        "cp_map": "CP application linker map.",
        "files": "Stable depset containing every public output.",
        "managed_app_image": "Managed app_ab_crc.rbl update image.",
        "partition_metadata": "Directory containing validated package metadata.",
        "recovery_image": "Combined all-app.bin recovery image.",
        "target": "BK target name.",
        "version": "Firmware version selected by the Bazel build setting.",
    },
)

_EXPECTED_PYTHON_VERSION = "3.11"
_EXPECTED_TOOLCHAIN_VERSION = "10.3.1"

def _native_firmware_resources(_os, _inputs_size):
    return {
        "cpu": 4,
        "memory": 4096,
    }

_TARGET_CONFIGS = {
    "bk7258": "//tools/bazel/platforms:is_bk7258",
}

def _bk7258_firmware_impl(ctx):
    output_root = ctx.label.name
    version = ctx.attr._firmware_version[FirmwareVersionInfo].value
    ap_elf = ctx.actions.declare_file(output_root + "/ap/firmware.elf")
    ap_map = ctx.actions.declare_file(output_root + "/ap/firmware.map")
    ap_image = ctx.actions.declare_file(output_root + "/ap/app.bin")
    cp_elf = ctx.actions.declare_file(output_root + "/cp/firmware.elf")
    cp_map = ctx.actions.declare_file(output_root + "/cp/firmware.map")
    cp_image = ctx.actions.declare_file(output_root + "/cp/app.bin")
    managed_app_image = ctx.actions.declare_file(output_root + "/app_ab_crc.rbl")
    recovery_image = ctx.actions.declare_file(output_root + "/all-app.bin")
    partition_metadata = ctx.actions.declare_directory(output_root + "/partition-metadata")
    outputs = [
        ap_elf,
        ap_map,
        ap_image,
        cp_elf,
        cp_map,
        cp_image,
        managed_app_image,
        recovery_image,
        partition_metadata,
    ]
    graph_sources = [
        dependency[H2NativeComponentInfo].files
        for dependency in ctx.attr.graph
    ]
    native_components = collect_native_components(ctx.attr.graph)
    for component in native_components:
        if component.execution_unit not in ["ap", "cp"]:
            fail("BK7258 native component %s must declare execution_unit ap or cp" % component.owner)
    prebuilt_components = collect_firmware_components(ctx.attr.graph)
    prebuilt_files = [component.archive for component in prebuilt_components]
    prebuilt_headers = [component.headers for component in prebuilt_components]
    inputs = depset(
        [ctx.file.project, ctx.file._archive_cmake, ctx.file._native_component_cmake, ctx.file._sdk_version, ctx.file._toolchain_archives, ctx.file._sdk_locator, ctx.file._toolchain_locator, ctx.file._ccache_locator, ctx.file.ram_regions] + ctx.files.srcs + ctx.files.support_files + ctx.files.project_support_files + ctx.files.ap_config + ctx.files.cp_config + [ctx.file.ap_gpio, ctx.file.cp_gpio] + prebuilt_files,
        transitive = graph_sources + prebuilt_headers,
    )

    args = ctx.actions.args()
    args.add("--source-root", ".")
    args.add("--project", ctx.file.project.path)
    args.add("--project-name", ctx.attr.project_name)
    args.add("--target", ctx.attr.target)
    args.add("--board", ctx.attr.board)
    args.add("--version", version)
    args.add("--sdk-version-file", ctx.file._sdk_version.path)
    args.add("--toolchain-archives-file", ctx.file._toolchain_archives.path)
    args.add("--sdk-locator", ctx.file._sdk_locator.path)
    args.add("--toolchain-locator", ctx.file._toolchain_locator.path)
    args.add("--ccache-runtime-locator", ctx.file._ccache_locator.path)
    args.add("--expected-python-version", _EXPECTED_PYTHON_VERSION)
    args.add("--expected-toolchain-version", _EXPECTED_TOOLCHAIN_VERSION)
    args.add("--ap-elf-output", ap_elf.path)
    args.add("--ap-map-output", ap_map.path)
    args.add("--ap-image-output", ap_image.path)
    args.add("--cp-elf-output", cp_elf.path)
    args.add("--cp-map-output", cp_map.path)
    args.add("--cp-image-output", cp_image.path)
    args.add("--managed-app-output", managed_app_image.path)
    args.add("--recovery-output", recovery_image.path)
    args.add("--partition-metadata-output", partition_metadata.path)
    args.add("--ram-regions", ctx.file.ram_regions.path)
    for support_file in ctx.files.support_files:
        args.add("--support-file", support_file.path)
    for support_file in ctx.files.project_support_files:
        args.add("--project-support-file", support_file.path)
    for config in ctx.files.ap_config:
        args.add("--ap-config", config.path)
    for config in ctx.files.cp_config:
        args.add("--cp-config", config.path)
    args.add("--ap-gpio", ctx.file.ap_gpio.path)
    args.add("--cp-gpio", ctx.file.cp_gpio.path)
    for component in prebuilt_components:
        args.add("--prebuilt-component", component.component_name + "=" + component.archive.path)
        if component.generate_component:
            args.add("--generate-prebuilt-component", component.component_name)
        for include_root in component.include_roots:
            args.add("--prebuilt-component-include", component.component_name + "=" + include_root)
    for component in native_components:
        component_key = "%s:%s" % (component.execution_unit, component.name)
        args.add("--native-component", component_key + "=" + component.directory)
        for component_file in component.files:
            args.add("--native-component-file", component_key + "=" + component_file.path)
        for source in component.srcs:
            args.add("--native-component-source", component_key + "=" + source.path)

    ctx.actions.run(
        arguments = [args],
        executable = ctx.executable._runner,
        execution_requirements = {
            "no-remote-exec": "1",
            "no-sandbox": "1",
        },
        inputs = inputs,
        mnemonic = "Bk7258Firmware",
        outputs = outputs,
        progress_message = "Building BK7258 firmware %{label}",
        resource_set = _native_firmware_resources,
        tools = [ctx.executable._runner],
        env = {"PATH": "/usr/bin:/bin"},
        use_default_shell_env = False,
    )

    files = depset(outputs)
    return [
        DefaultInfo(files = files),
        Bk7258FirmwareInfo(
            ap_elf = ap_elf,
            ap_image = ap_image,
            ap_map = ap_map,
            cp_elf = cp_elf,
            cp_image = cp_image,
            cp_map = cp_map,
            files = files,
            managed_app_image = managed_app_image,
            partition_metadata = partition_metadata,
            recovery_image = recovery_image,
            target = ctx.attr.target,
            version = version,
        ),
    ]

_bk7258_firmware = rule(
    implementation = _bk7258_firmware_impl,
    attrs = {
        "_runner": attr.label(
            default = "//tools/bazel:bk7258_runner",
            cfg = "exec",
            executable = True,
        ),
        "_archive_cmake": attr.label(
            allow_single_file = True,
            default = "//native_component_src/bk7258/ap/cmake:h2_bazel_archive.cmake",
        ),
        "_firmware_version": attr.label(default = "//tools/bazel:firmware_version"),
        "_sdk_version": attr.label(
            allow_single_file = True,
            default = "//tools/bazel:native_versions/bk7258_sdk_commit.txt",
        ),
        "_toolchain_archives": attr.label(
            allow_single_file = True,
            default = "//tools/bazel:native_versions/bk_toolchain_archives.txt",
        ),
        "_sdk_locator": attr.label(
            allow_single_file = True,
            default = "@gizos_bk7258_sdk//:locator.json",
        ),
        "_toolchain_locator": attr.label(
            allow_single_file = True,
            default = "@gizos_bk_arm_toolchain//:locator.json",
        ),
        "_ccache_locator": attr.label(
            allow_single_file = True,
            default = "@gizos_native_ccache_runtime//:locator.json",
        ),
        "_native_component_cmake": attr.label(
            allow_single_file = True,
            default = "//tools/bazel:native_component.cmake",
        ),
        "board": attr.string(mandatory = True),
        "graph": attr.label_list(
            aspects = [firmware_components_aspect],
            doc = "Launcher source graph analyzed for the selected firmware platform.",
            providers = [H2NativeComponentInfo],
        ),
        "project": attr.label(
            allow_single_file = ["CMakeLists.txt"],
            mandatory = True,
        ),
        "project_name": attr.string(mandatory = True),
        "project_support_files": attr.label_list(allow_files = True),
        "ram_regions": attr.label(
            allow_single_file = [".csv"],
            mandatory = True,
        ),
        "ap_config": attr.label_list(
            allow_files = [".defaults"],
            mandatory = True,
            doc = "Ordered AP Kconfig defaults merged in declaration order, later layers winning.",
        ),
        "cp_config": attr.label_list(
            allow_files = [".defaults"],
            mandatory = True,
            doc = "Ordered CP Kconfig defaults merged in declaration order, later layers winning.",
        ),
        "ap_gpio": attr.label(
            allow_single_file = [".h"],
            mandatory = True,
            doc = "AP usr_gpio_cfg.h staged into the AP configuration directory.",
        ),
        "cp_gpio": attr.label(
            allow_single_file = [".h"],
            mandatory = True,
            doc = "CP usr_gpio_cfg.h staged into the CP configuration directory.",
        ),
        "srcs": attr.label_list(
            allow_files = True,
            doc = "Project-local sources retained as explicit action inputs.",
        ),
        "support_files": attr.label_list(
            allow_files = True,
            doc = "Repository-relative launcher inputs needed outside the project directory.",
        ),
        "target": attr.string(
            mandatory = True,
            values = sorted(_TARGET_CONFIGS),
        ),
    },
)

def bk7258_firmware(name, target, **kwargs):
    """Declares one native BK7258 AP/CP firmware target.

    Args:
      name: Bazel target name.
      target: BK7258 target configuration name.
      **kwargs: Attributes forwarded to the private firmware rule.
    """
    if target not in _TARGET_CONFIGS:
        fail("unsupported BK target: " + target)
    if "target_compatible_with" in kwargs:
        fail("bk7258_firmware owns target_compatible_with")
    compatibility = select({
        "//tools/bazel/platforms:ci_graph": [],
        _TARGET_CONFIGS[target]: [],
        "//conditions:default": ["@platforms//:incompatible"],
    })
    _bk7258_firmware(
        name = name,
        target = target,
        target_compatible_with = compatibility,
        **kwargs
    )
