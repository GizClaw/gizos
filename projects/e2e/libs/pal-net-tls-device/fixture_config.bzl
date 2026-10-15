"""Only public isolated fixture/trust data is embedded, never Wi-Fi credentials."""

def _impl(ctx):
    lines = []
    for name in ["HOST", "SESSION", "CA_HEX", "WRONG_CA_HEX", "DNS_HOST", "DNS_IPV4"]:
        key = "H2_PAL_NET_TLS_" + name
        lines.append("#define %s %s" % (key, json.encode(ctx.var.get(key, ""))))
    for name in ["PORT", "EPOCH_MS"]:
        key = "H2_PAL_NET_TLS_" + name
        value = ctx.var.get(key, "0")
        if not value or any([character not in "0123456789" for character in value.elems()]):
            fail(key + " must be decimal")
        lines.append("#define %s %sULL" % (key, value))
    output = ctx.actions.declare_file("include/h2_pal_net_tls_fixture_config.h")
    ctx.actions.write(output, "\n".join(lines) + "\n")
    return [DefaultInfo(files = depset([output]))]

net_tls_fixture_config = rule(implementation = _impl)
