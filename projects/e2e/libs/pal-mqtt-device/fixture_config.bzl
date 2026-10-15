"""Compile explicit public LAN fixture inputs into the standalone MQTT image."""

def _impl(ctx):
    lines = []
    for name in ["HOST", "SESSION_PREFIX", "CA_PEM_HEX", "WRONG_CA_PEM_HEX"]:
        lines.append("#define H2_PAL_MQTT_%s %s" % (name, json.encode(ctx.var.get("H2_PAL_MQTT_" + name, ""))))
    for name in ["TCP_PORT", "TLS_PORT", "FIXTURE_EPOCH_MS"]:
        value = ctx.var.get("H2_PAL_MQTT_" + name, "0")
        if not value or any([character not in "0123456789" for character in value.elems()]):
            fail("H2_PAL_MQTT_%s must be decimal" % name)
        lines.append("#define H2_PAL_MQTT_%s %sULL" % (name, value))
    output = ctx.actions.declare_file("include/h2_pal_mqtt_fixture_config.h")
    ctx.actions.write(output, "\n".join(lines) + "\n")
    return [DefaultInfo(files = depset([output]))]

mqtt_fixture_config = rule(implementation = _impl)
