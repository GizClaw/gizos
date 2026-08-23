"""Complete JieLi (pi32v2) firmware builds backed by canonical devenv.

One toolchain repository serves every JieLi family; each family brings its own
SDK locator, `cpu/<target>` post-build contract and platform constraint, so the
rule is parameterised by `target` the same way `esp_idf_firmware` selects
`esp32s3`/`esp32p4`.
"""

load(":firmware.bzl", "FirmwareVersionInfo")
load(":firmware_components.bzl", "collect_firmware_components", "firmware_components_aspect")
load(":native_component.bzl", "H2NativeComponentInfo", "collect_native_components")

JieliFirmwareInfo = provider(
    doc = "Structured native outputs from one JieLi firmware build.",
    fields = {
        "board": "Physical board identity.",
        "elf": "Application ELF linked by the SDK.",
        "entry": "Canonical launcher package.",
        "files": "Stable depset containing every public output.",
        "flash_image": "jl_isd.bin full NOR flash image produced by isd_download.",
        "fw": "jl_isd.fw firmware container consumed by JieLi flashing tools.",
        "image": "Image identity.",
        "manifest": "Normalized native build manifest.",
        "symbols": "Sorted symbol size table exported by objsizedump.",
        "target": "JieLi SDK cpu target name (br23, wl82).",
        "update_image": "update.ufw upgrade package for USB disk, SD card and OTA upgrade.",
        "version": "Firmware version supplied to the native build.",
    },
)

# target -> (family, Linux-host config_setting, SDK locator, commit file,
#            post script)
_TARGETS = {
    "br23": struct(
        family = "ac695n",
        compatibility = "//tools/bazel/platforms:is_ac695n_linux_host",
        sdk_locator = "@h2_jieli_ac695n_sdk//:locator.json",
        sdk_version = "//tools/bazel:native_versions/jieli_ac695n_sdk_commit.txt",
        post_script = "//tools/bazel:jieli/local_post_br23.sh",
    ),
    "wl82": struct(
        family = "ac791n",
        compatibility = "//tools/bazel/platforms:is_ac791n_linux_host",
        sdk_locator = "@h2_jieli_ac791n_sdk//:locator.json",
        sdk_version = "//tools/bazel:native_versions/jieli_ac791n_sdk_commit.txt",
        post_script = "//tools/bazel:jieli/local_post_wl82.sh",
    ),
}

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

def _jieli_firmware_impl(ctx):
    output_root = ctx.label.name
    version = ctx.attr._firmware_version[FirmwareVersionInfo].value
    elf = ctx.actions.declare_file(output_root + "/firmware.elf")
    symbols = ctx.actions.declare_file(output_root + "/symbols.txt")
    flash_image = ctx.actions.declare_file(output_root + "/jl_isd.bin")
    fw = ctx.actions.declare_file(output_root + "/jl_isd.fw")
    update_image = ctx.actions.declare_file(output_root + "/update.ufw")
    manifest = ctx.actions.declare_file(output_root + "/manifest.json")
    outputs = [elf, symbols, flash_image, fw, update_image, manifest]
    graph_sources = [
        dependency[H2NativeComponentInfo].files
        for dependency in ctx.attr.graph
    ]
    native_components = collect_native_components(ctx.attr.graph)
    prebuilt_components = collect_firmware_components(ctx.attr.graph)
    prebuilt_headers = [component.headers for component in prebuilt_components]
    inputs = depset(
        [
            ctx.file.sdk_version,
            ctx.file.post_script,
            ctx.file._sdk_wrapper,
            ctx.file._toolchain_archives,
            ctx.file.sdk_locator,
            ctx.file._toolchain_locator,
            ctx.file._postbuild_locator,
        ] + [component.archive for component in prebuilt_components] + ctx.files.srcs,
        transitive = graph_sources + prebuilt_headers,
    )

    args = ctx.actions.args()
    args.add("--source-root", ".")
    args.add("--target", ctx.attr.target)
    args.add("--entry", ctx.label.package)
    args.add("--board", ctx.attr.board)
    args.add("--image", ctx.attr.image)
    args.add("--sdk-project", ctx.attr.sdk_project)
    if ctx.attr.sdk_entry_source:
        args.add("--sdk-entry-source", ctx.attr.sdk_entry_source)
    args.add("--sdk-wrapper", ctx.file._sdk_wrapper.path)
    args.add("--version", version)
    args.add("--sdk-version-file", ctx.file.sdk_version.path)
    args.add("--toolchain-archives-file", ctx.file._toolchain_archives.path)
    args.add("--sdk-locator", ctx.file.sdk_locator.path)
    args.add("--toolchain-locator", ctx.file._toolchain_locator.path)
    args.add("--postbuild-locator", ctx.file._postbuild_locator.path)
    args.add("--post-script", ctx.file.post_script.path)
    args.add("--elf-output", elf.path)
    args.add("--symbols-output", symbols.path)
    args.add("--flash-image-output", flash_image.path)
    args.add("--fw-output", fw.path)
    args.add("--update-output", update_image.path)
    args.add("--manifest-output", manifest.path)
    for component in native_components:
        args.add("--native-component", component.name)
        for include_root in component.include_roots:
            args.add("--native-include-root", include_root)
        for source in component.srcs:
            if _is_native_source(source.path):
                args.add("--native-component-source", "%s=%s" % (component.name, source.path))
    for component in prebuilt_components:
        args.add("--prebuilt-component", "%s=%s" % (component.component_name, component.archive.path))
        for include_root in component.include_roots:
            args.add("--native-include-root", include_root)

    ctx.actions.run(
        arguments = [args],
        executable = ctx.executable._runner,
        execution_requirements = {
            "no-remote-exec": "1",
            "no-sandbox": "1",
        },
        inputs = inputs,
        mnemonic = "JieliFirmware",
        outputs = outputs,
        progress_message = "Building JieLi %s firmware %%{label}" % ctx.attr.target,
        resource_set = _native_firmware_resources,
        tools = [ctx.executable._runner],
        env = {"PATH": "/usr/bin:/bin"},
        use_default_shell_env = False,
    )

    files = depset(outputs)
    return [
        DefaultInfo(files = files),
        OutputGroupInfo(release = files),
        JieliFirmwareInfo(
            board = ctx.attr.board,
            elf = elf,
            entry = ctx.label.package,
            files = files,
            flash_image = flash_image,
            fw = fw,
            image = ctx.attr.image,
            manifest = manifest,
            symbols = symbols,
            target = ctx.attr.target,
            update_image = update_image,
            version = version,
        ),
    ]

_jieli_firmware = rule(
    implementation = _jieli_firmware_impl,
    attrs = {
        "_firmware_version": attr.label(default = "//tools/bazel:firmware_version"),
        "_runner": attr.label(
            default = "//tools/bazel:jieli_runner",
            cfg = "exec",
            executable = True,
        ),
        "_toolchain_archives": attr.label(
            allow_single_file = True,
            default = "//tools/bazel:native_versions/jieli_toolchain_archives.txt",
        ),
        "_toolchain_locator": attr.label(
            allow_single_file = True,
            default = "@h2_jieli_toolchain//:locator.json",
        ),
        "_postbuild_locator": attr.label(
            allow_single_file = True,
            default = "@h2_jieli_postbuild//:locator.json",
        ),
        "_sdk_wrapper": attr.label(
            allow_single_file = [".mk"],
            default = "//tools/bazel:jieli/h2_sdk_wrapper.mk",
        ),
        "board": attr.string(mandatory = True),
        "graph": attr.label_list(
            aspects = [firmware_components_aspect],
            doc = "Launcher graph whose transitive sources invalidate the selected firmware action.",
            providers = [H2NativeComponentInfo],
        ),
        "image": attr.string(mandatory = True),
        "post_script": attr.label(
            allow_single_file = [".sh"],
            mandatory = True,
            doc = "Repository-owned local post-build script selected by the macro for the target.",
        ),
        "sdk_entry_source": attr.string(
            doc = "SDK-project-relative C file whose app_main symbol is renamed so the launcher component provides app_main.",
        ),
        "sdk_locator": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "sdk_project": attr.string(
            mandatory = True,
            doc = "SDK-root-relative directory whose Makefile builds the selected launcher ('.' for the SDK root).",
        ),
        "sdk_version": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "srcs": attr.label_list(
            allow_files = True,
            doc = "Entry-local files retained as explicit action inputs.",
        ),
        "target": attr.string(
            mandatory = True,
            values = sorted(_TARGETS),
        ),
    },
)

def jieli_firmware(name, target, **kwargs):
    """Declares one native JieLi firmware target.

    Args:
      name: Bazel target name.
      target: JieLi SDK cpu target (`br23` for AC695N, `wl82` for AC791N).
      **kwargs: Attributes forwarded to the private firmware rule.
    """
    if target not in _TARGETS:
        fail("unsupported JieLi target: " + target)
    if not kwargs.get("graph"):
        fail("jieli_firmware requires an explicit graph with the package-local launcher component")
    for owned in ("target_compatible_with", "post_script", "sdk_locator", "sdk_version"):
        if owned in kwargs:
            fail("jieli_firmware owns " + owned)
    selected = _TARGETS[target]
    compatibility = select({
        "//tools/bazel/platforms:ci_graph": [],
        selected.compatibility: [],
        "//conditions:default": ["@platforms//:incompatible"],
    })
    _jieli_firmware(
        name = name,
        post_script = selected.post_script,
        sdk_locator = selected.sdk_locator,
        sdk_version = selected.sdk_version,
        target = target,
        target_compatible_with = compatibility,
        **kwargs
    )
