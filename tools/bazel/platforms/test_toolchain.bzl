"""Host execution toolchain for tests of cross-compiled package artifacts."""

def _empty_test_toolchain_impl(_ctx):
    return [platform_common.ToolchainInfo()]

empty_test_toolchain = rule(
    implementation = _empty_test_toolchain_impl,
)
