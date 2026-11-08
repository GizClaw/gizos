"""Ephemeral server key material for the temporary AP image only."""

def _impl(ctx):
    output = ctx.actions.declare_file("include/h2_pal_ipv6_server_config.h")
    lines = []
    for name in ["LEAF_HEX", "EXPIRED_HEX", "KEY_HEX"]:
        key = "H2_PAL_IPV6_" + name
        lines.append("#define %s %s" % (key, json.encode(ctx.var.get(key, ""))))
    ctx.actions.write(output, "\n".join(lines) + "\n")
    return [DefaultInfo(files = depset([output]))]

ipv6_server_config = rule(implementation = _impl)
