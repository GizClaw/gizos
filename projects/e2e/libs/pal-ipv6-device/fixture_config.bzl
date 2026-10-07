"""Public isolated IPv6 peer/trust inputs; saved Wi-Fi credentials are never embedded."""

def _impl(ctx):
    lines = []
    for name in ["HOST", "SESSION", "CA_HEX", "WRONG_CA_HEX", "DNS_HOST", "DNS_IPV6", "HTTP_URL", "FALLBACK_URL", "OFFER_URL", "STUN_URL"]:
        key = "H2_PAL_IPV6_" + name
        lines.append("#define %s %s" % (key, json.encode(ctx.var.get(key, ""))))
    for name in ["PORT", "MQTT_PORT", "DNS_PORT", "EPOCH_MS", "BOARD_FIXTURE"]:
        key = "H2_PAL_IPV6_" + name
        value = ctx.var.get(key, "0")
        if not value or any([character not in "0123456789" for character in value.elems()]):
            fail(key + " must be decimal")
        lines.append("#define %s %sULL" % (key, value))
    output = ctx.actions.declare_file("include/h2_pal_ipv6_fixture_config.h")
    ctx.actions.write(output, "\n".join(lines) + "\n")
    return [DefaultInfo(files = depset([output]))]

ipv6_fixture_config = rule(implementation = _impl)
