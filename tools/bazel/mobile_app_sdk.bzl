"""Expose the exact SDK archive selected by an App's native split transition."""
_AppSDK = provider(fields = ["archives"])
_ATTRIBUTES = ["deps", "srcs", "archive", "shared_library", "static_library"]

def _collect(target, ctx):
    own = []
    if ctx.rule.kind == "_mobile_package":
        own = target[OutputGroupInfo].artifact.to_list()
    transitive = []
    for name in _ATTRIBUTES:
        values = getattr(ctx.rule.attr, name, [])
        if type(values) != "list":
            values = [values]
        for dependency in values:
            if type(dependency) == "Target" and _AppSDK in dependency:
                transitive.append(dependency[_AppSDK].archives)
    return [_AppSDK(archives = depset(own, transitive = transitive))]

_sdk_aspect = aspect(implementation = _collect, attr_aspects = _ATTRIBUTES)

def _impl(ctx):
    archives = ctx.attr.app[_AppSDK].archives
    if len(archives.to_list()) != 1:
        fail("Expected exactly one packaged SDK in the App graph: %s" % archives.to_list())
    return [DefaultInfo(files = archives)]

mobile_app_sdk = rule(
    implementation = _impl,
    attrs = {"app": attr.label(mandatory = True, aspects = [_sdk_aspect])},
)
