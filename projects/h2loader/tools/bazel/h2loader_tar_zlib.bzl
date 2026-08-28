"""Build one H2Loader archive from a standard native firmware provider."""

load("//tools/bazel:bk7258.bzl", "Bk7258FirmwareInfo")
load("//tools/bazel:esp_idf.bzl", "FirmwareInfo")

FirmwareReleaseInfo = provider(
    doc = "Canonical H2Loader release outputs for one firmware entry.",
    fields = {
        "board": "Physical board identity.",
        "entry": "Source-root-relative launcher entry.",
        "factory": "ESP Loader combined factory image or None.",
        "image": "Image identity.",
        "metadata": "Machine-readable metadata file.",
        "package": "The standard H2Loader package.",
        "platform": "Firmware platform family.",
        "recovery": "Loader recovery bundle or None.",
        "release_files": "Depset of release assets and metadata.",
        "role": "app or h2loader.",
        "target": "Chip target identity.",
        "version": "Firmware version selected by the build setting.",
    },
)

_IDENTITY_CHARACTERS = "0123456789abcdefghijklmnopqrstuvwxyz._-"

def _validate_identity(board, image, role, target):
    if role not in ("app", "h2loader"):
        fail("unsupported H2Loader firmware role: " + role)
    for name, value in (("board", board), ("image", image), ("target", target)):
        if not value or len(value) > 95:
            fail("H2Loader firmware %s must contain 1..95 characters" % name)
        for character in value.elems():
            if character not in _IDENTITY_CHARACTERS:
                fail("H2Loader firmware %s contains an unsafe character: %r" % (name, character))

def _native_firmware(ctx):
    target = ctx.attr.firmware
    if FirmwareInfo in target:
        firmware = target[FirmwareInfo]
        return struct(
            app_image = firmware.app_image,
            app_path = "app/esp/app.bin",
            factory_image = firmware.combined_factory_image,
            inputs = [firmware.app_image, firmware.elf, firmware.map, firmware.bootloader_image, firmware.combined_factory_image, firmware.partition_table_image, firmware.flash_files, firmware.flash_metadata],
            native_artifacts = [
                struct(name = "firmware.elf", file = firmware.elf),
                struct(name = "firmware.map", file = firmware.map),
                struct(name = "app.bin", file = firmware.app_image),
                struct(name = "bootloader.bin", file = firmware.bootloader_image),
                struct(name = "combined_factory.bin", file = firmware.combined_factory_image),
                struct(name = "partition-table.bin", file = firmware.partition_table_image),
                struct(name = "flasher_args.json", file = firmware.flash_metadata),
            ],
            platform = "esp",
            recovery_image = None,
            recovery_inputs = [firmware.flash_files, firmware.flash_metadata],
            target = firmware.target,
            version = firmware.version,
        )
    if Bk7258FirmwareInfo in target:
        firmware = target[Bk7258FirmwareInfo]
        return struct(
            app_image = firmware.managed_app_image,
            app_path = "app/bk/app_ab_crc.rbl",
            factory_image = None,
            inputs = [firmware.ap_elf, firmware.ap_map, firmware.ap_image, firmware.cp_elf, firmware.cp_map, firmware.cp_image, firmware.managed_app_image, firmware.recovery_image, firmware.partition_metadata],
            native_artifacts = [
                struct(name = "ap/firmware.elf", file = firmware.ap_elf),
                struct(name = "ap/firmware.map", file = firmware.ap_map),
                struct(name = "ap/app.bin", file = firmware.ap_image),
                struct(name = "cp/firmware.elf", file = firmware.cp_elf),
                struct(name = "cp/firmware.map", file = firmware.cp_map),
                struct(name = "cp/app.bin", file = firmware.cp_image),
                struct(name = "app_ab_crc.rbl", file = firmware.managed_app_image),
                struct(name = "all-app.bin", file = firmware.recovery_image),
            ],
            platform = "bk7258",
            recovery_image = firmware.recovery_image,
            recovery_inputs = [firmware.recovery_image],
            target = firmware.target,
            version = firmware.version,
        )
    fail("firmware must provide FirmwareInfo or Bk7258FirmwareInfo")

def _h2loader_tar_zlib_impl(ctx):
    firmware = _native_firmware(ctx)
    if firmware.target != ctx.attr.target:
        fail("package target %s does not match firmware target %s" % (ctx.attr.target, firmware.target))
    if ctx.attr.role == "h2loader" and ctx.files.package_data:
        fail("h2loader firmware cannot declare package_data")

    stem = "%s-%s-%s" % (ctx.attr.board, ctx.attr.image, ctx.attr.target)
    package = ctx.actions.declare_file(ctx.label.name + "/" + stem + ".update.tar.zlib")
    metadata = ctx.actions.declare_file(ctx.label.name + "/" + stem + ".firmware.json")
    factory = None
    recovery = None
    if ctx.attr.role == "h2loader":
        recovery = ctx.actions.declare_file(ctx.label.name + "/" + stem + ".recovery.h2fb")
        if firmware.factory_image:
            factory = ctx.actions.declare_file(ctx.label.name + "/" + stem + ".combined_factory.bin")

    args = ctx.actions.args()
    args.add("--source-root", ".")
    args.add("--app-image", firmware.app_image.path)
    args.add("--app-path", firmware.app_path)
    args.add("--entry", ctx.label.package)
    args.add("--platform", firmware.platform)
    args.add("--board", ctx.attr.board)
    args.add("--image", ctx.attr.image)
    args.add("--role", ctx.attr.role)
    args.add("--target", ctx.attr.target)
    args.add("--version", firmware.version)
    args.add("--package-output", package.path)
    args.add("--metadata-output", metadata.path)
    if factory:
        args.add("--factory-image", firmware.factory_image.path)
        args.add("--factory-output", factory.path)
    if recovery:
        args.add("--recovery", recovery.path)
        if firmware.platform == "esp":
            native = ctx.attr.firmware[FirmwareInfo]
            args.add("--esp-flash-root", native.flash_files.path)
            args.add("--esp-flash-metadata", native.flash_metadata.path)
        else:
            args.add("--bk-recovery-image", firmware.recovery_image.path)
            args.add("--bk-recovery-config", ctx.file.recovery_config.path)
    if ctx.attr.package_data_root:
        args.add("--package-data-root", ctx.attr.package_data_root)
    for data_file in ctx.files.package_data:
        args.add("--package-data-file", data_file.path)
    for native in firmware.native_artifacts:
        args.add("--native-artifact", "%s=%s" % (native.name, native.file.path))

    action_inputs = firmware.inputs + ctx.files.package_data
    if recovery and ctx.file.recovery_config:
        action_inputs.append(ctx.file.recovery_config)
    ctx.actions.run(
        arguments = [args],
        executable = ctx.executable._runner,
        inputs = depset(action_inputs),
        mnemonic = "H2LoaderTarZlib",
        outputs = [package, metadata] + ([factory] if factory else []) + ([recovery] if recovery else []),
        progress_message = "Packaging H2Loader archive %{label}",
        tools = [ctx.executable._runner],
    )

    release_files = depset([package, metadata] + ([factory] if factory else []) + ([recovery] if recovery else []))
    return [
        DefaultInfo(files = release_files),
        OutputGroupInfo(release = release_files),
        FirmwareReleaseInfo(
            board = ctx.attr.board,
            entry = ctx.label.package,
            factory = factory,
            image = ctx.attr.image,
            metadata = metadata,
            package = package,
            platform = firmware.platform,
            recovery = recovery,
            release_files = release_files,
            role = ctx.attr.role,
            target = ctx.attr.target,
            version = firmware.version,
        ),
    ]

_h2loader_tar_zlib = rule(
    implementation = _h2loader_tar_zlib_impl,
    attrs = {
        "_runner": attr.label(default = "//projects/h2loader/tools/bazel:h2loader_tar_zlib_runner", cfg = "exec", executable = True),
        "board": attr.string(mandatory = True),
        "firmware": attr.label(mandatory = True),
        "image": attr.string(mandatory = True),
        "package_data": attr.label_list(allow_files = True),
        "package_data_root": attr.string(),
        "role": attr.string(mandatory = True, values = ["app", "h2loader"]),
        "recovery_config": attr.label(allow_single_file = [".json"]),
        "target": attr.string(mandatory = True),
    },
)

def h2loader_tar_zlib(name, board, image, role, target, recovery_config = None, **kwargs):
    """Packages one standard native firmware target for H2Loader installation."""
    _validate_identity(board, image, role, target)
    if bool(kwargs.get("package_data")) != bool(kwargs.get("package_data_root")):
        fail("package_data and package_data_root must be declared together")
    if recovery_config != None and not (role == "h2loader" and target == "bk7258"):
        fail("recovery_config is only valid for BK7258 H2Loader firmware")
    if role == "h2loader" and target == "bk7258" and recovery_config == None:
        fail("BK7258 H2Loader firmware requires recovery_config")
    _h2loader_tar_zlib(
        name = name,
        board = board,
        image = image,
        role = role,
        target = target,
        recovery_config = recovery_config,
        **kwargs
    )
