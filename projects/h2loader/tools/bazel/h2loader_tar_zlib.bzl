"""Build parallel format-1 and format-2 packages from shared native inputs."""

load("//tools/bazel:bk7258.bzl", "Bk7258FirmwareInfo")
load("//tools/bazel:esp_idf.bzl", "FirmwareInfo")
load("//tools/bazel:jieli.bzl", "JieliFirmwareInfo")

FirmwareReleaseInfo = provider(
    doc = "Canonical H2Loader release outputs for one firmware entry.",
    fields = {
        "board": "Physical board identity.",
        "entry": "Source-root-relative launcher entry.",
        "factory": "ESP Loader combined factory image or None.",
        "image": "Image identity.",
        "metadata": "Machine-readable metadata file.",
        "package": "The standard H2Loader package.",
        "package_format": "Managed wire format: 1 for tar_zlib, 2 for zlib_tar.",
        "native": "Borrowed native firmware inputs for a sibling package action.",
        "package_data": "Borrowed data files for a sibling package action.",
        "package_data_root": "Original data path root for canonical identities.",
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
    if ctx.attr.source:
        return ctx.attr.source[FirmwareReleaseInfo].native
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
            inputs = [firmware.ap_elf, firmware.ap_map, firmware.ap_image, firmware.cp_elf, firmware.cp_map, firmware.cp_image, firmware.managed_app_image, firmware.recovery_image, firmware.partition_metadata] + ([firmware.recovery_config] if firmware.recovery_config else []),
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
            recovery_config = firmware.recovery_config,
            recovery_image = firmware.recovery_image,
            recovery_inputs = [firmware.recovery_image] + ([firmware.recovery_config] if firmware.recovery_config else []),
            target = firmware.target,
            version = firmware.version,
        )
    if JieliFirmwareInfo in target:
        firmware = target[JieliFirmwareInfo]
        if firmware.target != "wl82" and (firmware.target != "br35" or ctx.attr.role != "app"):
            fail("JieLi packaging supports WL82 managed firmware and BR35 Apps only")
        return struct(
            app_image = firmware.update_image,
            app_path = "app/jieli/update.ufw",
            factory_image = None,
            inputs = firmware.files.to_list(),
            native_artifacts = [
                struct(name = "firmware.elf", file = firmware.elf),
                struct(name = "symbols.txt", file = firmware.symbols),
                struct(name = "jl_isd.bin", file = firmware.flash_image),
                struct(name = "jl_isd.fw", file = firmware.fw),
                struct(name = "update.ufw", file = firmware.update_image),
                struct(name = "manifest.json", file = firmware.manifest),
            ],
            platform = "jieli",
            recovery_image = None,
            recovery_inputs = [],
            target = "ac707n" if firmware.target == "br35" else firmware.target,
            version = firmware.version,
        )
    fail("firmware must provide FirmwareInfo, Bk7258FirmwareInfo or JieliFirmwareInfo")

def _package_impl(ctx, package_format):
    firmware = _native_firmware(ctx)
    source = ctx.attr.source[FirmwareReleaseInfo] if ctx.attr.source else None
    board = source.board if source else ctx.attr.board
    image = source.image if source else ctx.attr.image
    role = source.role if source else ctx.attr.role
    target = source.target if source else ctx.attr.target
    package_data = source.package_data if source else ctx.files.package_data
    data_root = source.package_data_root if source else ctx.attr.package_data_root
    if firmware.target != target:
        fail("package target %s does not match firmware target %s" % (target, firmware.target))
    if role == "h2loader" and package_data:
        fail("h2loader firmware cannot declare package_data")

    stem = "%s-%s-%s" % (board, image, target)
    suffix = ".update.tar.zlib" if package_format == 1 else ".update.tar"
    package = ctx.actions.declare_file(ctx.label.name + "/" + stem + suffix)
    metadata = ctx.actions.declare_file(ctx.label.name + "/" + stem + ".firmware.json")
    factory = None
    recovery = None
    if role == "h2loader" and firmware.platform in ("esp", "bk7258"):
        recovery = ctx.actions.declare_file(ctx.label.name + "/" + stem + ".recovery.h2fb")
        if firmware.factory_image:
            factory = ctx.actions.declare_file(ctx.label.name + "/" + stem + ".combined_factory.bin")

    args = ctx.actions.args()
    args.add("--source-root", ".")
    args.add("--app-image", firmware.app_image.path)
    args.add("--app-path", firmware.app_path)
    args.add("--entry", ctx.label.package)
    args.add("--platform", firmware.platform)
    args.add("--board", board)
    args.add("--image", image)
    args.add("--role", role)
    args.add("--target", target)
    args.add("--version", firmware.version)
    args.add("--package-output", package.path)
    args.add("--package-format", package_format)
    args.add("--metadata-output", metadata.path)
    if factory:
        args.add("--factory-image", firmware.factory_image.path)
        args.add("--factory-output", factory.path)
    if recovery:
        args.add("--recovery", recovery.path)
        if firmware.platform == "esp":
            args.add("--esp-flash-root", firmware.recovery_inputs[0].path)
            args.add("--esp-flash-metadata", firmware.recovery_inputs[1].path)
        else:
            if not firmware.recovery_config:
                fail("BK7258 H2Loader firmware must provide its layout recovery config")
            args.add("--bk-recovery-image", firmware.recovery_image.path)
            args.add("--bk-recovery-config", firmware.recovery_config.path)
    if data_root:
        args.add("--package-data-root", data_root)
    for data_file in package_data:
        # The short path is the entry's repository-relative name and the path is
        # where its bytes are, so package data may be produced by a rule instead
        # of checked in under the declared root. They are separate arguments
        # because either may contain any character a filename may contain.
        args.add("--package-data-file", data_file.short_path)
        args.add("--package-data-source", data_file.path)
    for native in firmware.native_artifacts:
        args.add("--native-artifact", "%s=%s" % (native.name, native.file.path))

    action_inputs = firmware.inputs + package_data
    ctx.actions.run(
        arguments = [args],
        executable = ctx.executable._runner,
        inputs = depset(action_inputs),
        mnemonic = "H2LoaderTarZlib" if package_format == 1 else "H2LoaderZlibTar",
        outputs = [package, metadata] + ([factory] if factory else []) + ([recovery] if recovery else []),
        progress_message = "Packaging H2Loader archive %{label}",
        tools = [ctx.executable._runner],
    )

    release_files = depset([package, metadata] + ([factory] if factory else []) + ([recovery] if recovery else []))
    return [
        DefaultInfo(files = release_files),
        OutputGroupInfo(release = release_files),
        FirmwareReleaseInfo(
            board = board,
            entry = ctx.label.package,
            factory = factory,
            image = image,
            metadata = metadata,
            package = package,
            package_format = package_format,
            native = firmware,
            package_data = package_data,
            package_data_root = data_root,
            platform = firmware.platform,
            recovery = recovery,
            release_files = release_files,
            role = role,
            target = target,
            version = firmware.version,
        ),
    ]

def _tar_zlib_impl(ctx):
    return _package_impl(ctx, 1)

def _zlib_tar_impl(ctx):
    return _package_impl(ctx, 2)

def _attrs():
    return {
        "_runner": attr.label(default = "//projects/h2loader/tools/bazel:h2loader_tar_zlib_runner", cfg = "exec", executable = True),
        "board": attr.string(),
        "firmware": attr.label(),
        "image": attr.string(),
        "package_data": attr.label_list(allow_files = True),
        "package_data_root": attr.string(),
        "role": attr.string(),
        "source": attr.label(providers = [FirmwareReleaseInfo]),
        "target": attr.string(),
    }

_h2loader_tar_zlib = rule(implementation = _tar_zlib_impl, attrs = _attrs())
_h2loader_zlib_tar = rule(implementation = _zlib_tar_impl, attrs = _attrs())

def h2loader_tar_zlib(name, board, image, role, target, **kwargs):
    """Preserve the original labels and format-1 monolithic zlib output."""
    _validate_identity(board, image, role, target)
    if "source" in kwargs or not kwargs.get("firmware"):
        fail("tar_zlib requires a raw firmware target")
    if bool(kwargs.get("package_data")) != bool(kwargs.get("package_data_root")):
        fail("package_data and package_data_root must be declared together")
    _h2loader_tar_zlib(name = name, board = board, image = image, role = role, target = target, **kwargs)

def h2loader_zlib_tar(name, source, **kwargs):
    """Produce format 2 independently using an existing package's raw inputs.

    Only source analysis metadata is borrowed. The action consumes shared raw
    firmware/data files, so building this target does not build the old package.
    """
    for key in ("firmware", "board", "image", "role", "target", "package_data", "package_data_root", "package_format"):
        if key in kwargs:
            fail("zlib_tar inherits %s from source" % key)
    _h2loader_zlib_tar(name = name, source = source, **kwargs)
