"""Local Linux build rules for ARM64 Rockchip images. See README.md for the host contract."""

_EXEC_GROUP = "native_linux"
_GROUPS = {_EXEC_GROUP: exec_group(exec_compatible_with = ["@platforms//os:linux"])}
_NATIVE = {
    "source_date_epoch": attr.int(mandatory = True, doc = "Fixed Unix timestamp for the image tools."),
    "_runner": attr.label(default = Label(":runner.py"), allow_single_file = True),
}
_SOURCE = {
    "archive": attr.label(mandatory = True, allow_single_file = True, doc = "Pinned source tarball with one leading directory."),
    "patches": attr.label_list(allow_files = True, doc = "Ordered patches applied with GNU patch -p1 and zero fuzz."),
}
_KBUILD = {
    "build_host": attr.string(default = "builder"),
    "build_user": attr.string(default = "builder"),
}

def _stage(ctx, stage, inputs, outputs, params):
    if ctx.attr.source_date_epoch < 0:
        fail("source_date_epoch must be nonnegative")
    args = ctx.actions.args()
    params["outputs"] = [output.path for output in outputs]
    args.add_all(["-I", ctx.file._runner.path, stage, ctx.info_file.path, json.encode(params)])
    ctx.actions.run(
        executable = "/usr/bin/python3",
        arguments = [args],
        inputs = [ctx.file._runner, ctx.info_file] + inputs,
        outputs = outputs,
        mnemonic = "Rockchip" + stage,
        env = {
            "LC_ALL": "C",
            "PATH": "/usr/sbin:/usr/bin:/sbin:/bin",
            "SOURCE_DATE_EPOCH": str(ctx.attr.source_date_epoch),
            "TZ": "UTC",
        },
        exec_group = _EXEC_GROUP,
        execution_requirements = {"no-remote": "1", "no-sandbox": "1"},
    )
    return [DefaultInfo(files = depset(outputs))]

def _source(ctx):
    return {
        "archive": ctx.file.archive.path,
        "patches": [f.path for f in ctx.files.patches],
    }

def _kbuild(ctx):
    return {"build_user": ctx.attr.build_user, "build_host": ctx.attr.build_host}

def _mapping(targets):
    entries = []
    for target, destination in targets.items():
        files = target.files.to_list()
        if len(files) != 1 or files[0].is_directory:
            fail("%s must provide exactly one file for %s" % (target.label, destination))
        entries.append([files[0].path, destination])
    return entries

def _kernel(ctx):
    params = _source(ctx) | _kbuild(ctx) | {
        "config": ctx.file.config.path,
        "dts_files": _mapping(ctx.attr.dts_files),
        "dtb_target": ctx.attr.dtb_target,
    }
    inputs = [ctx.file.archive, ctx.file.config] + ctx.files.patches + ctx.files.dts_files
    return _stage(ctx, "Kernel", inputs, [ctx.outputs.image, ctx.outputs.dtb], params)

rockchip_linux_kernel = rule(
    implementation = _kernel,
    doc = "Build an ARM64 Linux Image and a selected Kbuild DTB from a source archive.",
    exec_groups = _GROUPS,
    attrs = _NATIVE | _SOURCE | _KBUILD | {
        "config": attr.label(mandatory = True, allow_single_file = True, doc = "Base .config for olddefconfig."),
        "dts_files": attr.label_keyed_string_dict(allow_files = True, doc = "Optional source labels mapped to paths under arch/arm64/boot/dts/."),
        "dtb_target": attr.string(mandatory = True, doc = "Kbuild DTB target relative to arch/arm64/boot/dts/."),
        "image": attr.output(mandatory = True),
        "dtb": attr.output(mandatory = True),
    },
)

def _uboot(ctx):
    outputs = [ctx.outputs.idbloader, ctx.outputs.uboot, ctx.outputs.trust, ctx.outputs.script]
    inputs = [ctx.file.archive, ctx.file.defconfig, ctx.file.ddr, ctx.file.miniloader, ctx.file.bl31, ctx.file.trust_ini, ctx.file.boot_command]
    if ctx.file.trust_checksum:
        inputs.append(ctx.file.trust_checksum)
    params = _source(ctx) | _kbuild(ctx) | {
        "defconfig": ctx.file.defconfig.path,
        "ddr": ctx.file.ddr.path,
        "miniloader": ctx.file.miniloader.path,
        "bl31": ctx.file.bl31.path,
        "trust_ini": ctx.file.trust_ini.path,
        "trust_checksum": ctx.file.trust_checksum.path if ctx.file.trust_checksum else None,
        "boot_command": ctx.file.boot_command.path,
        "script_name": ctx.attr.script_name,
        "mkimage_soc": ctx.attr.mkimage_soc,
        "load_address": ctx.attr.load_address,
    }
    return _stage(ctx, "LegacyUBoot", inputs + ctx.files.patches, outputs, params)

rockchip_legacy_uboot = rule(
    implementation = _uboot,
    doc = "Build the Hardkernel-style AArch64 U-Boot, rksd DDR/miniloader, BL3X trust and legacy boot script.",
    exec_groups = _GROUPS,
    attrs = _NATIVE | _SOURCE | _KBUILD | {
        "defconfig": attr.label(mandatory = True, allow_single_file = True),
        "ddr": attr.label(mandatory = True, allow_single_file = True),
        "miniloader": attr.label(mandatory = True, allow_single_file = True),
        "bl31": attr.label(mandatory = True, allow_single_file = True),
        "trust_ini": attr.label(mandatory = True, allow_single_file = True, doc = "trust_merger input using bl31.elf and output trust.img."),
        "trust_checksum": attr.label(allow_single_file = True, doc = "Optional expected SHA-256 for the complete trust.img."),
        "boot_command": attr.label(mandatory = True, allow_single_file = True),
        "script_name": attr.string(mandatory = True, doc = "Image name in the legacy script header (up to 32 ASCII bytes)."),
        "mkimage_soc": attr.string(mandatory = True, doc = "mkimage -n chip identifier for the rksd ID block."),
        "load_address": attr.string(mandatory = True, doc = "32-bit hex U-Boot payload address for loaderimage."),
        "idbloader": attr.output(mandatory = True),
        "uboot": attr.output(mandatory = True),
        "trust": attr.output(mandatory = True),
        "script": attr.output(mandatory = True),
    },
)

def _rootfs(ctx):
    params = _source(ctx) | {
        "config": ctx.file.config.path,
        "files": _mapping(ctx.attr.files),
        "executables": _mapping(ctx.attr.executables),
        "source_files": ctx.attr.source_files,
        "directories": ctx.attr.directories,
    }
    inputs = [ctx.file.archive, ctx.file.config] + ctx.files.patches + ctx.files.files + ctx.files.executables
    return _stage(ctx, "BusyBoxSquashfs", inputs, [ctx.outputs.out], params)

busybox_squashfs = rule(
    implementation = _rootfs,
    doc = "Build a static AArch64 BusyBox root filesystem with caller-supplied files and directories.",
    exec_groups = _GROUPS,
    attrs = _NATIVE | _SOURCE | {
        "config": attr.label(mandatory = True, allow_single_file = True, doc = "BusyBox options over allnoconfig, including CONFIG_STATIC=y."),
        "files": attr.label_keyed_string_dict(allow_files = True, doc = "Data labels mapped to root-relative destinations, mode 0644."),
        "executables": attr.label_keyed_string_dict(allow_files = True, doc = "Executable labels mapped to root-relative destinations, mode 0755."),
        "source_files": attr.string_dict(doc = "Files from the unpacked BusyBox source mapped to root-relative destinations, mode 0644."),
        "directories": attr.string_list(doc = "Root-relative directories to create."),
        "out": attr.output(mandatory = True),
    },
)

def _disk(ctx):
    params = {
        "layout": ctx.file.layout.path,
        "inputs": _mapping(ctx.attr.inputs),
        "layout_image": ctx.attr.layout_image,
    }
    return _stage(ctx, "SDImage", [ctx.file.layout] + ctx.files.inputs, [ctx.outputs.out], params)

rockchip_sd_image = rule(
    implementation = _disk,
    doc = "Assemble a raw card file using an explicit genimage layout and named input payloads.",
    exec_groups = _GROUPS,
    attrs = _NATIVE | {
        "layout": attr.label(mandatory = True, allow_single_file = True),
        "inputs": attr.label_keyed_string_dict(allow_files = True, doc = "Single-file labels mapped to paths in genimage's input directory."),
        "layout_image": attr.string(mandatory = True, doc = "Name of the final image entry in the layout."),
        "out": attr.output(mandatory = True),
    },
)
