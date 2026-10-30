"""An explicit public device identity for a factory BLE peer."""

load("@rules_cc//cc:defs.bzl", "cc_library")

def _config_impl(ctx):
    uid = ctx.var.get("H2_MFG_PEER_DUT_UID", "")
    if uid and (len(uid) != 12 or any([c not in "0123456789abcdef" for c in uid.elems()])):
        fail("H2_MFG_PEER_DUT_UID must be 12 lowercase hexadecimal digits")
    address = ctx.var.get("H2_MFG_PEER_DUT_BLE_ADDRESS", uid)
    if address and (len(address) != 12 or any([c not in "0123456789abcdef" for c in address.elems()])):
        fail("H2_MFG_PEER_DUT_BLE_ADDRESS must be 12 lowercase hexadecimal digits")
    source = ctx.actions.declare_file(ctx.label.name + ".c")
    ctx.actions.write(source, (
        "const char *h2_mfg_peer_dut_uid(void) { return %s; }\n" +
        "const char *h2_mfg_peer_dut_ble_address(void) { return %s; }\n"
    ) % (json.encode(uid), json.encode(address)))
    return [DefaultInfo(files = depset([source]))]

_config = rule(implementation = _config_impl)

def mfg_peer_config(name):
    _config(name = name + "_source")
    cc_library(name = name, srcs = [":" + name + "_source"])
