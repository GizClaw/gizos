"""Complete BK3633 firmware builds backed by canonical devenv."""

load(":firmware.bzl", "FirmwareVersionInfo")
load(":firmware_components.bzl", "collect_firmware_components", "firmware_components_aspect")
load(":native_component.bzl", "H2NativeComponentInfo", "collect_native_components")

Bk3633FirmwareInfo = provider(
    doc = "Structured native outputs from one BK3633 firmware build.",
    fields = {
        "app_image": "Application binary image.",
        "board": "Physical board identity.",
        "elf": "Application ELF.",
        "entry": "Canonical launcher package.",
        "files": "Stable depset containing every public output.",
        "image": "Image identity.",
        "manifest": "Normalized native build manifest.",
        "map": "Application linker map.",
        "recovery_image": "Complete merge CRC image for direct flash recovery.",
        "stack": "BK3633 Stack flavor.",
        "target": "BK target name.",
        "version": "Firmware version supplied to the native build.",
    },
)

_EXPECTED_TOOLCHAIN_VERSION = "10.3.1"

def _native_firmware_resources(_os, _inputs_size):
    return {
        "cpu": 4,
        "memory": 4096,
    }

def _is_native_source(path):
    return any([path.endswith(extension) for extension in [
        ".c",
        ".cc",
        ".cpp",
        ".cxx",
        ".s",
        ".S",
    ]])

def _bk3633_firmware_impl(ctx):
    output_root = ctx.label.name
    version = ctx.attr._firmware_version[FirmwareVersionInfo].value
    elf = ctx.actions.declare_file(output_root + "/firmware.elf")
    app_image = ctx.actions.declare_file(output_root + "/app.bin")
    map_file = ctx.actions.declare_file(output_root + "/firmware.map")
    recovery_image = ctx.actions.declare_file(output_root + "/merge-crc.bin")
    manifest = ctx.actions.declare_file(output_root + "/manifest.json")
    outputs = [elf, app_image, map_file, recovery_image, manifest]
    graph_sources = [
        dependency[H2NativeComponentInfo].files
        for dependency in ctx.attr.graph
    ]
    native_components = collect_native_components(ctx.attr.graph)
    prebuilt_components = collect_firmware_components(ctx.attr.graph)
    prebuilt_headers = [component.headers for component in prebuilt_components]
    inputs = depset(
        [ctx.file.project, ctx.file._sdk_version, ctx.file._toolchain_archives, ctx.file._sdk_locator, ctx.file._toolchain_locator, ctx.file._ccache_locator] +
        [component.archive for component in prebuilt_components] + ctx.files.srcs,
        transitive = graph_sources + prebuilt_headers,
    )

    args = ctx.actions.args()
    args.add("--source-root", ".")
    args.add("--project", ctx.file.project.path)
    args.add("--entry", ctx.label.package)
    args.add("--board", ctx.attr.board)
    args.add("--image", ctx.attr.image)
    args.add("--native-target", ctx.attr.native_target)
    args.add("--native-merge", ctx.attr.native_merge)
    args.add("--version", version)
    args.add("--sdk-version-file", ctx.file._sdk_version.path)
    args.add("--toolchain-archives-file", ctx.file._toolchain_archives.path)
    args.add("--sdk-locator", ctx.file._sdk_locator.path)
    args.add("--toolchain-locator", ctx.file._toolchain_locator.path)
    args.add("--ccache-runtime-locator", ctx.file._ccache_locator.path)
    args.add("--expected-toolchain-version", _EXPECTED_TOOLCHAIN_VERSION)
    args.add("--binconverter", ctx.executable._binconverter.path)
    args.add("--elf-output", elf.path)
    args.add("--app-output", app_image.path)
    args.add("--map-output", map_file.path)
    args.add("--recovery-output", recovery_image.path)
    args.add("--manifest-output", manifest.path)
    for component in prebuilt_components:
        args.add("--prebuilt-component", "%s=%s" % (component.component_name, component.archive.path))
        for include_root in component.include_roots:
            args.add("--native-include-root", include_root)
    for component in native_components:
        for include_root in component.include_roots:
            args.add("--native-include-root", include_root)
        for source in component.srcs:
            if _is_native_source(source.path):
                args.add("--native-component-source", "%s=%s" % (component.name, source.path))
    if ctx.attr.rwip_link_probe:
        args.add("--rwip-link-probe")

    ctx.actions.run(
        arguments = [args],
        executable = ctx.executable._runner,
        execution_requirements = {
            "no-remote-exec": "1",
            "no-sandbox": "1",
        },
        inputs = inputs,
        mnemonic = "Bk3633Firmware",
        outputs = outputs,
        progress_message = "Building BK3633 firmware %{label}",
        resource_set = _native_firmware_resources,
        tools = [ctx.executable._runner, ctx.executable._binconverter],
        env = {"PATH": "/usr/bin:/bin"},
        use_default_shell_env = False,
    )

    files = depset(outputs)
    return [
        DefaultInfo(files = files),
        OutputGroupInfo(release = files),
        Bk3633FirmwareInfo(
            app_image = app_image,
            board = ctx.attr.board,
            elf = elf,
            entry = ctx.label.package,
            files = files,
            image = ctx.attr.image,
            manifest = manifest,
            map = map_file,
            recovery_image = recovery_image,
            stack = "allroles",
            target = "bk3633",
            version = version,
        ),
    ]

_bk3633_firmware = rule(
    implementation = _bk3633_firmware_impl,
    attrs = {
        "_binconverter": attr.label(
            default = "//tools/bk3633_binconverter:binconverter",
            cfg = "exec",
            executable = True,
        ),
        "_firmware_version": attr.label(default = "//tools/bazel:firmware_version"),
        "_runner": attr.label(
            default = "//tools/bazel:bk3633_runner",
            cfg = "exec",
            executable = True,
        ),
        "_sdk_version": attr.label(
            allow_single_file = True,
            default = "//tools/bazel:native_versions/bk3633_sdk_commit.txt",
        ),
        "_toolchain_archives": attr.label(
            allow_single_file = True,
            default = "//tools/bazel:native_versions/bk_toolchain_archives.txt",
        ),
        "_sdk_locator": attr.label(
            allow_single_file = True,
            default = "@h2_bk3633_sdk//:locator.json",
        ),
        "_toolchain_locator": attr.label(
            allow_single_file = True,
            default = "@gizos_bk_arm_toolchain//:locator.json",
        ),
        "_ccache_locator": attr.label(
            allow_single_file = True,
            default = "@gizos_native_ccache_runtime//:locator.json",
        ),
        "board": attr.string(mandatory = True),
        "graph": attr.label_list(
            aspects = [firmware_components_aspect],
            doc = "Launcher graph whose transitive sources invalidate the selected firmware action.",
            providers = [H2NativeComponentInfo],
        ),
        "image": attr.string(mandatory = True),
        "native_merge": attr.string(mandatory = True),
        "native_target": attr.string(mandatory = True),
        "project": attr.label(
            allow_single_file = ["Makefile"],
            mandatory = True,
        ),
        "rwip_link_probe": attr.bool(default = False),
        "srcs": attr.label_list(
            allow_files = True,
            doc = "Project-local sources retained as explicit action inputs.",
        ),
    },
)

def bk3633_firmware(name, **kwargs):
    """Declares one native BK3633 allroles firmware target.

    Args:
      name: Bazel target name.
      **kwargs: Attributes forwarded to the private firmware rule.
    """
    if "target_compatible_with" in kwargs:
        fail("bk3633_firmware owns target_compatible_with")
    compatibility = select({
        "//tools/bazel/platforms:ci_graph": [],
        "//tools/bazel/platforms:is_bk3633": [],
        "//conditions:default": ["@platforms//:incompatible"],
    })
    _bk3633_firmware(
        name = name,
        target_compatible_with = compatibility,
        **kwargs
    )
