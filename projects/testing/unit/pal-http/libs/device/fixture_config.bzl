"""Compile explicit, public fixture endpoints and test trust into device Apps."""

def _fixture_config_impl(ctx):
    values = []
    for name in ["HTTP_BASE", "HTTPS_BASE", "UNTRUSTED_HTTPS_BASE", "CA_PEM_HEX"]:
        key = "H2_PAL_HTTP_" + name
        values.append("#define %s %s" % (key, json.encode(ctx.var.get(key, ""))))
    epoch = ctx.var.get("H2_PAL_HTTP_FIXTURE_EPOCH_MS", "0")
    if not epoch or any([character not in "0123456789" for character in epoch.elems()]):
        fail("H2_PAL_HTTP_FIXTURE_EPOCH_MS must be decimal milliseconds")
    values.append("#define H2_PAL_HTTP_FIXTURE_EPOCH_MS %sULL" % epoch)
    output = ctx.actions.declare_file("include/h2_pal_http_fixture_config.h")
    ctx.actions.write(output, "\n".join(values) + "\n")
    return [DefaultInfo(files = depset([output]))]

http_fixture_config = rule(implementation = _fixture_config_impl)
