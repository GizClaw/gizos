"""Explicit public endpoints for the isolated device WebRTC fixture."""
def _fixture_config_impl(ctx):
    values = []
    for name in ["OFFER_URL", "STUN_URL"]:
        key = "H2_PAL_WEBRTC_" + name
        values.append("#define %s %s" % (key, json.encode(ctx.var.get(key, ""))))
    output = ctx.actions.declare_file("include/h2_pal_webrtc_fixture_config.h")
    ctx.actions.write(output, "\n".join(values) + "\n")
    return [DefaultInfo(files = depset([output]))]
webrtc_fixture_config = rule(implementation = _fixture_config_impl)
