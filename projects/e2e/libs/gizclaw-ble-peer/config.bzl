"""Public, explicit bench identity; no credential is a build input."""

load("@rules_cc//cc:defs.bzl", "cc_library")

def _impl(ctx):
    uid = ctx.var.get("H2_BLE_PEER_DUT_UID", "")
    origin = ctx.var.get("H2_BLE_PEER_ORIGIN", "")
    if uid and (len(uid) != 12 or any([c not in "0123456789abcdef" for c in uid.elems()])):
        fail("H2_BLE_PEER_DUT_UID must be the 12-digit lowercase BLE UID")
    if origin and (not origin.startswith("https://") or len(origin) > 192 or any([c in origin[8:] for c in ["/", "?", "#", "@", " "]])):
        fail("H2_BLE_PEER_ORIGIN must be an HTTPS origin without path/userinfo")
    src = ctx.actions.declare_file(ctx.label.name + ".c")
    contract = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(src, "const char *h2_ble_peer_dut_uid(void) { return %s; }\nconst char *h2_ble_peer_origin(void) { return %s; }\n" % (json.encode(uid), json.encode(origin)))
    ctx.actions.write(contract, json.encode({"dut_uid": uid, "origin": origin}) + "\n")
    return [DefaultInfo(files = depset([src])), OutputGroupInfo(contract = depset([contract]))]

_config = rule(implementation = _impl)

def ble_peer_config(name):
    _config(name = name + "_source")
    cc_library(name = name, srcs = [":" + name + "_source"])
