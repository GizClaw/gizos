"""Validate workload selection before assembling a Mosaico E2E image."""

_SUITES = [
    "core",
    "storage",
    "display",
    "audio",
    "libco",
    "camera",
    "json",
    "crypto",
    "audio-decoder",
    "net-tls",
    "http",
    "mqtt",
    "webrtc",
    "wifi",
    "ipv6",
    "atomic",
    "iperf",
    "iperf-client",
    "iperf-server",
    "lua-runtime",
    "lua-link",
    "webrtc-performance",
    "aec-calibration",
]

def _impl(ctx):
    suite = ctx.var.get("mosaico_e2e", "core")
    if suite not in _SUITES:
        fail("Unsupported mosaico_e2e=%s. Select an E2E workload, not a board diagnostic, product Loader, or chip compile check. Allowed: %s" % (suite, ", ".join(_SUITES)))
    selected = ctx.actions.declare_file(ctx.label.name + ".txt")
    ctx.actions.write(selected, suite + "\n")
    return [DefaultInfo(files = depset([selected]))]

mosaico_e2e_suite = rule(implementation = _impl)
