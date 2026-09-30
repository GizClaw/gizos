"""Bind a non-secret, operator-selected AppConfig fixture to every launcher."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo", "string_flag")
load("@rules_cc//cc:defs.bzl", "cc_library")

def _fixture_impl(ctx):
    key = ctx.attr.key[BuildSettingInfo].value
    profile = ctx.attr.profile[BuildSettingInfo].value
    value = ctx.attr.value[BuildSettingInfo].value
    endpoint = ctx.attr.endpoint[BuildSettingInfo].value
    token = ctx.attr.token[BuildSettingInfo].value
    time_server = ctx.attr.time_server[BuildSettingInfo].value
    allowed = "abcdefghijklmnopqrstuvwxyz0123456789.-"
    if len(key) > 63 or any([c not in allowed for c in key.elems()]) or (key and key[0] not in "abcdefghijklmnopqrstuvwxyz"):
        fail("AppConfig fixture key must be a 1-63 byte lowercase alias, or empty when not configured")
    if len(profile) > 63 or any([c not in allowed for c in profile.elems()]):
        fail("fixture RuntimeProfile must be a lowercase resource ID")
    if len(value) > 4096:
        fail("AppConfig expected value exceeds the server fixture limit")
    if len(endpoint) >= 128 or len(token) > 4096:
        fail("board service input exceeds the portable configuration limit")
    if len(time_server) > 253 or any([c not in allowed for c in time_server.elems()]):
        fail("Time fixture must be an explicit DNS hostname or IPv4 address")
    source = ctx.actions.declare_file(ctx.label.name + ".c")
    contract = ctx.actions.declare_file(ctx.label.name + ".json")
    ctx.actions.write(source, "\n".join([
        "const char *h2_gizclaw_e2e_fixture_key(void) { return %s; }" % json.encode(key),
        "const char *h2_gizclaw_e2e_fixture_profile(void) { return %s; }" % json.encode(profile),
        "const char *h2_gizclaw_e2e_fixture_value(void) { return %s; }" % json.encode(value),
        "const char *h2_gizclaw_e2e_fixture_endpoint(void) { return %s; }" % json.encode(endpoint),
        "const char *h2_gizclaw_e2e_fixture_token(void) { return %s; }" % json.encode(token),
        "const char *h2_gizclaw_e2e_fixture_time_server(void) { return %s; }" % json.encode(time_server),
    ]) + "\n")
    ctx.actions.write(contract, json.encode({"app_config_key": key, "runtime_profile": profile, "app_config_value": value, "time_server": time_server}) + "\n")
    return [
        DefaultInfo(files = depset([source, contract])),
        OutputGroupInfo(source = depset([source]), contract = depset([contract])),
    ]

_fixture = rule(
    implementation = _fixture_impl,
    attrs = {
        "key": attr.label(mandatory = True, providers = [BuildSettingInfo]),
        "profile": attr.label(mandatory = True, providers = [BuildSettingInfo]),
        "value": attr.label(mandatory = True, providers = [BuildSettingInfo]),
        "endpoint": attr.label(mandatory = True, providers = [BuildSettingInfo]),
        "token": attr.label(mandatory = True, providers = [BuildSettingInfo]),
        "time_server": attr.label(mandatory = True, providers = [BuildSettingInfo]),
    },
)

def gizclaw_e2e_fixture(name, app_config_key = "", runtime_profile = "", app_config_value = "", server_endpoint = "", registration_token = "", time_server = ""):
    """Empty is buildable but RPC/all launchers reject it before networking.

    Supply the public profile key via the macro argument or the generated
    `<name>_key` Bazel string flag. Registration credentials remain runtime inputs.
    """
    string_flag(name = name + "_key", build_setting_default = app_config_key)
    string_flag(name = name + "_profile", build_setting_default = runtime_profile)
    string_flag(name = name + "_value", build_setting_default = app_config_value)
    # Board launchers have no process environment. These explicit build inputs
    # are omitted from the JSON/audit contract and are never committed defaults.
    string_flag(name = name + "_endpoint", build_setting_default = server_endpoint)
    string_flag(name = name + "_token", build_setting_default = registration_token)
    string_flag(name = name + "_time_server", build_setting_default = time_server)
    _fixture(name = name + "_generated", key = ":" + name + "_key", profile = ":" + name + "_profile", value = ":" + name + "_value", endpoint = ":" + name + "_endpoint", token = ":" + name + "_token", time_server = ":" + name + "_time_server")
    native.filegroup(name = name + "_contract", srcs = [":" + name + "_generated"], output_group = "contract")
    native.filegroup(name = name + "_source", srcs = [":" + name + "_generated"], output_group = "source")
    cc_library(
        name = name,
        srcs = [":" + name + "_source"],
    )
