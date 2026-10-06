"""Run a native Atomic package with host Python and the host H2Loader CLI."""

load("@rules_python//python:defs.bzl", "py_test")

def _host_cli_impl(ctx):
    return [DefaultInfo(files = depset([ctx.executable.cli]), runfiles = ctx.attr.cli[DefaultInfo].default_runfiles)]

_host_cli = rule(implementation = _host_cli_impl, attrs = {
    "cli": attr.label(default = "//projects/h2loader/targets/cc_binary/cli:h2loader", cfg = "exec", executable = True),
})

def _package_impl(ctx):
    files = [f for f in ctx.attr.package[DefaultInfo].files.to_list() if (f.basename.endswith(".update.tar") or f.basename.endswith(".update.tar.zlib"))]
    if len(files) != 1:
        fail("Atomic package must produce one update archive")
    return [DefaultInfo(files = depset(files))]

_package = rule(implementation = _package_impl, attrs = {"package": attr.label(mandatory = True)})

def atomic_device_test(name, package, target):
    artifact = name + "_package"
    cli = name + "_cli"
    _package(name = artifact, package = package)
    _host_cli(name = cli)
    py_test(
        name = name,
        srcs = ["//projects/e2e/libs/atomic-device:run_device.py"],
        main = "//projects/e2e/libs/atomic-device:run_device.py",
        args = [
            "$(rootpath :" + artifact + ")",
            "$(rootpath :" + cli + ")",
            "$(rootpath //projects/e2e/apps/atomic/app:include/h2_atomic_cases.inc)",
            target,
        ],
        data = [":" + artifact, ":" + cli, "//projects/e2e/apps/atomic/app:include/h2_atomic_cases.inc"],
        env_inherit = ["H2_ATOMIC_DEVICE_PORT", "H2_ATOMIC_DEVICE_UID"],
        local = True,
        legacy_create_init = False,
        tags = ["manual", "external"],
        timeout = "eternal",
    )
