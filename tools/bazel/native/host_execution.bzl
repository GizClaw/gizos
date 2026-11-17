"""Explicit runtime contract for Linux x86-64 vendor tools on an ARM64 host."""

X86_64_RUNTIME_ENVIRON = ["GIZMOTH_X86_64_TRANSLATION", "QEMU_LD_PREFIX"]

def linux_x86_64_runtime(repository_ctx):
    """Record the managed runtime only on Linux ARM64; never change host identity.

    GizMoth sets the versioned contract after registering qemu-x86_64 with
    binfmt_misc and mounting its x86-64 userspace. Tool versions and payload
    identities are still checked by the caller. Other ARM64 hosts fail closed.
    """
    if repository_ctx.os.name.lower() != "linux" or repository_ctx.os.arch.lower() not in ("aarch64", "arm64"):
        return {}
    contract = repository_ctx.os.environ.get("GIZMOTH_X86_64_TRANSLATION", "")
    if contract != "qemu-binfmt-v1":
        fail("UNSUPPORTED: Linux ARM64 x86-64 vendor tools require GIZMOTH_X86_64_TRANSLATION=qemu-binfmt-v1 after verified binfmt registration")
    prefix = repository_ctx.os.environ.get("QEMU_LD_PREFIX", "")
    if not prefix.startswith("/"):
        fail("QEMU_LD_PREFIX must name the managed x86-64 userspace by absolute path")
    root = repository_ctx.path(prefix)
    if not root.exists or not root.is_dir:
        fail("managed x86-64 userspace is unavailable: %s" % root)
    return {"kind": contract, "ld_prefix": str(root.realpath)}

def x86_64_runtime_environment(runtime):
    """Return only the environment needed by the registered QEMU interpreter."""
    return {"QEMU_LD_PREFIX": runtime["ld_prefix"]} if runtime else {}
