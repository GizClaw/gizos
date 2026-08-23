"""Complete ESP-IDF firmware builds backed by canonical devenv."""

load(":firmware.bzl", "FirmwareVersionInfo")
load(":firmware_components.bzl", "collect_firmware_components", "firmware_components_aspect")
load(":native_component.bzl", "H2NativeComponentInfo", "collect_native_components")

FirmwareInfo = provider(
    doc = "Structured native outputs from one ESP-IDF firmware build.",
    fields = {
        "app_image": "Application binary image.",
        "bootloader_image": "ESP-IDF bootloader image.",
        "combined_factory_image": "ESP-IDF combined image flashed at offset 0x0.",
        "elf": "Application ELF used for symbols and backtraces.",
        "files": "Stable depset containing every public output.",
        "flash_files": "Directory containing every image referenced by flash metadata.",
        "flash_metadata": "Normalized ESP-IDF flasher_args.json.",
        "map": "Application linker map.",
        "partition_table_image": "ESP-IDF partition-table image.",
        "target": "ESP chip target name.",
        "version": "Firmware version selected by the Bazel build setting.",
    },
)

def _native_firmware_resources(_os, _inputs_size):
    return {
        "cpu": 4,
        "memory": 4096,
    }

_TARGET_CONFIGS = {
    "esp32c5": "//tools/bazel/platforms:is_esp32c5",
    "esp32p4": "//tools/bazel/platforms:is_esp32p4",
    "esp32s3": "//tools/bazel/platforms:is_esp32s3",
}

def _esp_idf_firmware_impl(ctx):
    output_root = ctx.label.name
    version = ctx.attr._firmware_version[FirmwareVersionInfo].value
    elf = ctx.actions.declare_file(output_root + "/firmware.elf")
    map_file = ctx.actions.declare_file(output_root + "/firmware.map")
    app_image = ctx.actions.declare_file(output_root + "/app.bin")
    bootloader_image = ctx.actions.declare_file(output_root + "/bootloader.bin")
    combined_factory_image = ctx.actions.declare_file(output_root + "/combined_factory.bin")
    partition_table_image = ctx.actions.declare_file(output_root + "/partition-table.bin")
    flash_files = ctx.actions.declare_directory(output_root + "/flash-files")
    flash_metadata = ctx.actions.declare_file(output_root + "/flasher_args.json")
    outputs = [
        elf,
        map_file,
        app_image,
        bootloader_image,
        combined_factory_image,
        partition_table_image,
        flash_files,
        flash_metadata,
    ]
    support_files = ctx.files.support_files
    graph_sources = [
        dependency[H2NativeComponentInfo].files
        for dependency in ctx.attr.graph
    ]
    native_components = collect_native_components(ctx.attr.graph)
    prebuilt_components = collect_firmware_components(ctx.attr.graph)
    prebuilt_files = [component.archive for component in prebuilt_components]
    prebuilt_headers = [component.headers for component in prebuilt_components]
    inputs = depset(
        [ctx.file.project, ctx.file.partition, ctx.file._archive_cmake, ctx.file._native_component_cmake, ctx.file._sdk_version, ctx.file._tool_versions, ctx.file._sdk_locator, ctx.file._tools_locator, ctx.file._ccache_locator] + ctx.files.srcs + support_files + ctx.files.project_support_files + prebuilt_files,
        transitive = graph_sources + prebuilt_headers,
    )

    args = ctx.actions.args()
    args.add("--source-root", ".")
    args.add("--project", ctx.file.project.path)
    args.add("--partition", ctx.file.partition.path)
    args.add("--project-name", ctx.attr.project_name)
    args.add("--target", ctx.attr.target)
    args.add("--board", ctx.attr.board)
    args.add("--version", version)
    args.add("--idf-version-file", ctx.file._sdk_version.path)
    args.add("--idf-tool-versions-file", ctx.file._tool_versions.path)
    args.add("--idf-sdk-locator", ctx.file._sdk_locator.path)
    args.add("--idf-tools-locator", ctx.file._tools_locator.path)
    args.add("--ccache-runtime-locator", ctx.file._ccache_locator.path)
    if ctx.attr.h2loader_wifi_environment:
        args.add("--h2loader-wifi-environment")
    args.add("--elf-output", elf.path)
    args.add("--map-output", map_file.path)
    args.add("--app-output", app_image.path)
    args.add("--bootloader-output", bootloader_image.path)
    args.add("--combined-factory-output", combined_factory_image.path)
    args.add("--partition-table-output", partition_table_image.path)
    args.add("--flash-files-output", flash_files.path)
    args.add("--flash-metadata-output", flash_metadata.path)
    for support_file in support_files:
        args.add("--support-file", support_file.path)
    for support_file in ctx.files.project_support_files:
        args.add("--project-support-file", support_file.path)
    for name in ctx.attr.cmake_variables:
        value = ctx.var.get(name)
        if value != None:
            args.add("--cmake-variable", name + "=" + value)
    args.add("--support-file", ctx.file._native_component_cmake.path)
    for component in prebuilt_components:
        args.add("--prebuilt-component", component.component_name + "=" + component.archive.path)
        if component.generate_component:
            args.add("--generate-prebuilt-component", component.component_name)
        for include_root in component.include_roots:
            args.add("--prebuilt-component-include", component.component_name + "=" + include_root)
    for component in native_components:
        if component.execution_unit:
            fail("ESP-IDF component %s declares unsupported execution unit %s" % (
                component.name,
                component.execution_unit,
            ))
        args.add("--native-component", component.name + "=" + component.directory)
        for source in component.srcs:
            args.add("--native-component-source", component.name + "=" + source.path)

    action_environment = {"PATH": "/usr/bin:/bin"}
    if ctx.attr.h2loader_wifi_environment:
        credentials = ctx.configuration.default_shell_env.get("H2LOADER_WIFI_CREDENTIALS")
        if credentials != None:
            action_environment["H2LOADER_WIFI_CREDENTIALS"] = credentials

    ctx.actions.run(
        arguments = [args],
        executable = ctx.executable._runner,
        execution_requirements = {
            "no-remote-exec": "1",
            "no-sandbox": "1",
        },
        inputs = inputs,
        mnemonic = "EspIdfFirmware",
        outputs = outputs,
        progress_message = "Building ESP-IDF firmware %{label}",
        resource_set = _native_firmware_resources,
        tools = [ctx.executable._runner],
        env = action_environment,
        use_default_shell_env = False,
    )

    files = depset(outputs)
    return [
        DefaultInfo(files = files),
        FirmwareInfo(
            app_image = app_image,
            bootloader_image = bootloader_image,
            combined_factory_image = combined_factory_image,
            elf = elf,
            files = files,
            flash_files = flash_files,
            flash_metadata = flash_metadata,
            map = map_file,
            partition_table_image = partition_table_image,
            target = ctx.attr.target,
            version = version,
        ),
    ]

_esp_idf_firmware = rule(
    implementation = _esp_idf_firmware_impl,
    attrs = {
        "_runner": attr.label(
            default = "//tools/bazel:esp_idf_runner",
            cfg = "exec",
            executable = True,
        ),
        "_archive_cmake": attr.label(
            allow_single_file = True,
            default = "//native_component_src/esp-idf6.x/cmake:h2_bazel_archive.cmake",
        ),
        "_firmware_version": attr.label(default = "//tools/bazel:firmware_version"),
        "_sdk_version": attr.label(
            allow_single_file = True,
            default = "//tools/bazel:native_versions/esp_idf_commit.txt",
        ),
        "_tool_versions": attr.label(
            allow_single_file = True,
            default = "//tools/bazel:native_versions/esp_idf_tool_versions.txt",
        ),
        "_native_component_cmake": attr.label(
            allow_single_file = True,
            default = "//tools/bazel:native_component.cmake",
        ),
        "_sdk_locator": attr.label(
            allow_single_file = True,
            default = "@gizos_esp_idf_sdk//:locator.json",
        ),
        "_tools_locator": attr.label(
            allow_single_file = True,
            default = "@gizos_esp_idf_tools//:locator.json",
        ),
        "_ccache_locator": attr.label(
            allow_single_file = True,
            default = "@gizos_native_ccache_runtime//:locator.json",
        ),
        "board": attr.string(mandatory = True),
        "cmake_variables": attr.string_list(
            doc = "Allowlisted --define values forwarded as CMake cache variables.",
        ),
        "graph": attr.label_list(
            aspects = [firmware_components_aspect],
            doc = "Launcher source graph analyzed for the selected firmware platform.",
            providers = [H2NativeComponentInfo],
        ),
        "h2loader_wifi_environment": attr.bool(
            default = False,
            doc = "Forward allowlisted H2Loader Wi-Fi build inputs to this firmware only.",
        ),
        "project": attr.label(
            allow_single_file = ["CMakeLists.txt"],
            mandatory = True,
        ),
        "project_name": attr.string(mandatory = True),
        "project_support_files": attr.label_list(
            allow_files = True,
            doc = "Repository inputs copied by basename into the launcher project root.",
        ),
        "partition": attr.label(
            allow_single_file = [".csv"],
            mandatory = True,
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

def esp_idf_firmware(name, target, **kwargs):
    """Declares one native ESP-IDF firmware target.

    The target remains visible to the repository-wide CI graph configuration,
    while normal builds require the matching ESP firmware platform.

    Args:
      name: Bazel target name.
      target: ESP-IDF chip target name.
      **kwargs: Attributes forwarded to the private firmware rule.
    """
    if target not in _TARGET_CONFIGS:
        fail("unsupported ESP-IDF target: " + target)
    if "target_compatible_with" in kwargs:
        fail("esp_idf_firmware owns target_compatible_with")
    compatibility = select({
        "//tools/bazel/platforms:ci_graph": [],
        _TARGET_CONFIGS[target]: [],
        "//conditions:default": ["@platforms//:incompatible"],
    })
    _esp_idf_firmware(
        name = name,
        target = target,
        target_compatible_with = compatibility,
        **kwargs
    )
