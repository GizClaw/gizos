"""Local Linux build actions for a Rockchip SD boot image."""

def _stage(ctx, mnemonic, script, inputs, outputs):
    ctx.actions.run(
        executable = "/bin/bash",
        arguments = [ctx.file._run.path, script.path] + [f.path for f in outputs + inputs],
        inputs = [ctx.file._run, script] + inputs,
        outputs = outputs,
        mnemonic = mnemonic,
        env = {
            "LANG": "C",
            "PATH": "/usr/sbin:/usr/bin:/sbin:/bin",
            "SOURCE_DATE_EPOCH": "1704067200",
            "TZ": "UTC",
        },
        # The documented native Linux host compiler and image tools are host inputs.
        execution_requirements = {"no-remote": "1", "no-sandbox": "1"},
    )

def _impl(ctx):
    sources = {f.basename: f for f in ctx.files.upstream}
    intermediate = ctx.label.name + ".parts/"
    loader = [ctx.actions.declare_file(intermediate + n) for n in ["idbloader.img", "uboot.img", "trust.img", "boot.scr"]]
    kernel = [ctx.actions.declare_file(intermediate + n) for n in ["Image", "r36s.dtb"]]
    root = ctx.actions.declare_file(intermediate + "rootfs.sqfs")
    _stage(ctx, "RockchipUBoot", ctx.file._uboot, [sources[n] for n in ["uboot.tar.gz", "uboot.patch", "rocknix_rk3326_defconfig", "ddr.bin", "miniloader.bin", "bl31.elf"]] + [ctx.file.boot_script], loader)
    _stage(ctx, "RockchipLinux", ctx.file._linux, [sources["linux.tar.xz"], ctx.file.kernel_config] + [sources[n] for n in ["gpio.patch", "polldev.patch", "dts.patch", "panel.patch", "joypad.patch"]], kernel)
    _stage(ctx, "RockchipRootfs", ctx.file._rootfs, [sources["busybox.tar.bz2"], ctx.file.busybox_config, ctx.file.inittab, ctx.file.rc_sysinit, sources["rkbin.LICENSE"]], [root])
    img = ctx.actions.declare_file(ctx.label.name + ".img")
    _stage(ctx, "RockchipSDImage", ctx.file._disk, [ctx.file.layout] + loader + kernel + [root], [img])
    return [DefaultInfo(files = depset([img]))]

r36s_sd_image = rule(
    implementation = _impl,
    attrs = {
        "boot_script": attr.label(mandatory = True, allow_single_file = True),
        "busybox_config": attr.label(mandatory = True, allow_single_file = True),
        "inittab": attr.label(mandatory = True, allow_single_file = True),
        "kernel_config": attr.label(mandatory = True, allow_single_file = True),
        "layout": attr.label(mandatory = True, allow_single_file = True),
        "rc_sysinit": attr.label(mandatory = True, allow_single_file = True),
        "upstream": attr.label(mandatory = True, allow_files = True),
        "_disk": attr.label(default = ":disk.sh", allow_single_file = True),
        "_linux": attr.label(default = ":linux.sh", allow_single_file = True),
        "_rootfs": attr.label(default = ":rootfs.sh", allow_single_file = True),
        "_run": attr.label(default = ":run.sh", allow_single_file = True),
        "_uboot": attr.label(default = ":uboot.sh", allow_single_file = True),
    },
)
