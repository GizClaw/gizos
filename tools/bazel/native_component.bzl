"""Explicit source ownership for components compiled by native SDK builds."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load(":firmware_components.bzl", "FirmwareLibComponentInfo")

def _declared_include_root(root):
    path = str(root)
    return path not in ("", ".") and not path.startswith("bazel-out/")

H2NativeComponentInfo = provider(
    doc = "Sources and build metadata owned by one native SDK component.",
    fields = {
        "components": "Ordered native component registration records.",
        "files": "Stable depset of the component's transitive repository inputs.",
    },
)

H2NativeComponentsInfo = provider(
    doc = "Native component inputs reachable from a firmware dependency graph.",
    fields = {
        "components": "Ordered native component registration records.",
        "files": "Stable depset of native component repository inputs.",
    },
)

def _native_components_aspect_impl(target, ctx):
    if H2NativeComponentInfo in target:
        component = target[H2NativeComponentInfo]
        return [H2NativeComponentsInfo(
            components = component.components,
            files = component.files,
        )]
    if FirmwareLibComponentInfo in target:
        return [H2NativeComponentsInfo(components = [], files = depset())]

    transitive = []
    components = []
    for name in ["srcs", "hdrs", "textual_hdrs", "data", "deps", "implementation_deps", "runtime_deps", "exports"]:
        if not hasattr(ctx.rule.attr, name):
            continue
        dependencies = getattr(ctx.rule.attr, name)
        if type(dependencies) != "list":
            dependencies = [dependencies]
        for dependency in dependencies:
            if H2NativeComponentsInfo in dependency:
                transitive.append(dependency[H2NativeComponentsInfo].files)
                components.extend(dependency[H2NativeComponentsInfo].components)
            elif DefaultInfo in dependency:
                transitive.append(dependency[DefaultInfo].files)
    return [H2NativeComponentsInfo(
        components = components,
        files = depset(transitive = transitive),
    )]

native_components_aspect = aspect(
    implementation = _native_components_aspect_impl,
    attr_aspects = ["srcs", "hdrs", "textual_hdrs", "data", "deps", "implementation_deps", "runtime_deps", "exports"],
)

def _firmware_native_component_impl(ctx):
    transitive = []
    components = []
    include_roots = {root: True for root in ctx.attr.include_roots}
    for header in ctx.files.hdrs:
        include_roots[header.dirname] = True
    for dependency in ctx.attr.deps:
        if H2NativeComponentInfo in dependency:
            transitive.append(dependency[H2NativeComponentInfo].files)
            components.extend(dependency[H2NativeComponentInfo].components)
        elif FirmwareLibComponentInfo in dependency:
            # The firmware archive collector owns this dependency's action
            # inputs and native component registration.
            continue
        elif CcInfo in dependency:
            # Native compilation consumes public repository headers, not a
            # host/cross archive emitted for a different build boundary.
            compilation_context = dependency[CcInfo].compilation_context
            transitive.append(compilation_context.headers)
            for header in compilation_context.headers.to_list():
                include_roots[header.dirname] = True
            for roots in [
                compilation_context.includes,
                compilation_context.quote_includes,
                compilation_context.system_includes,
                compilation_context.framework_includes,
            ]:
                for root in roots.to_list():
                    if _declared_include_root(root):
                        include_roots[str(root)] = True
        elif H2NativeComponentsInfo in dependency:
            transitive.append(dependency[H2NativeComponentsInfo].files)
            components.extend(dependency[H2NativeComponentsInfo].components)
        elif DefaultInfo in dependency:
            # Source-only vendor overlays are ordinary file targets.
            transitive.append(dependency[DefaultInfo].files)
    files = depset(
        direct = ctx.files.srcs + ctx.files.native_srcs + ctx.files.hdrs + ctx.files.data,
        transitive = transitive,
    )
    direct_files = _unique_files(ctx.files.srcs + ctx.files.native_srcs + ctx.files.hdrs + ctx.files.data)
    cmake_directories = {
        file.dirname: True
        for file in direct_files
        if file.basename == "CMakeLists.txt"
    }
    component_name = ctx.attr.component_name
    component_directory = ctx.attr.component_directory
    if not component_name and len(cmake_directories) == 1:
        component_directory = cmake_directories.keys()[0]
        component_name = component_directory.rsplit("/", 1)[-1]
    if component_name:
        component_directory = component_directory or ctx.label.package
        components.append(struct(
            directory = component_directory,
            execution_unit = ctx.attr.execution_unit,
            files = direct_files,
            hdrs = _unique_files(ctx.files.hdrs),
            include_roots = sorted(include_roots),
            name = component_name,
            owner = str(ctx.label),
            srcs = _unique_files(ctx.files.srcs + ctx.files.native_srcs),
        ))
    return [
        DefaultInfo(files = files),
        H2NativeComponentInfo(components = components, files = files),
    ]

firmware_native_component = rule(
    implementation = _firmware_native_component_impl,
    attrs = {
        "data": attr.label_list(
            allow_files = True,
            doc = "Native build metadata and other non-source inputs owned by this component.",
        ),
        "component_directory": attr.string(
            doc = "Repository-relative native SDK component directory; defaults to this Bazel package.",
        ),
        "component_name": attr.string(
            doc = "Native SDK component name. Empty means this target is source ownership only.",
        ),
        "execution_unit": attr.string(
            default = "",
            doc = "Optional native execution unit, such as ap or cp.",
            values = ["", "ap", "cp"],
        ),
        "include_roots": attr.string_list(
            doc = "Repository-relative include roots exported to native build manifests.",
        ),
        "native_srcs": attr.label_list(
            allow_files = True,
            doc = "Direct compilable sources owned outside this Bazel package.",
        ),
        "deps": attr.label_list(
            aspects = [native_components_aspect],
            doc = "Other native components or Bazel archives required by this component.",
        ),
        "hdrs": attr.label_list(
            allow_files = True,
            doc = "Headers owned by this component.",
        ),
        "srcs": attr.label_list(
            allow_files = True,
            doc = "Sources owned by this component.",
        ),
    },
)

def _component_key(component):
    return "%s:%s" % (component.execution_unit, component.name)

def _unique_files(files):
    by_path = {}
    for file in files:
        by_path[file.path] = file
    return [by_path[path] for path in sorted(by_path)]

def _file_paths(files):
    return "\n".join(sorted([file.path for file in files]))

def _component_fingerprint(component):
    return "\n--\n".join([
        component.directory,
        _file_paths(component.files),
        _file_paths(component.srcs),
        _file_paths(component.hdrs),
        "\n".join(sorted(component.include_roots)),
    ])

def collect_native_components(graph):
    """Returns the validated, stable native component closure for a firmware graph.

    Args:
      graph: Dependencies carrying ``H2NativeComponentInfo`` providers.

    Returns:
      Stable native component descriptors ordered by execution unit and name.
    """
    by_key = {}
    source_owners = {}
    for dependency in graph:
        for component in dependency[H2NativeComponentInfo].components:
            key = _component_key(component)
            previous = by_key.get(key)
            if previous:
                if previous.owner != component.owner:
                    fail("native component %s is owned by both %s and %s" % (
                        key,
                        previous.owner,
                        component.owner,
                    ))
                if _component_fingerprint(previous) != _component_fingerprint(component):
                    fail("native component %s is owned by both %s and %s with different descriptors" % (
                        key,
                        previous.owner,
                        component.owner,
                    ))
                continue
            by_key[key] = component
            for source in component.srcs:
                previous_owner = source_owners.get(source.path)
                if previous_owner and previous_owner != component.owner:
                    fail("native source %s is owned by both %s and %s" % (
                        source.path,
                        previous_owner,
                        component.owner,
                    ))
                source_owners[source.path] = component.owner

    return [by_key[key] for key in sorted(by_key)]
