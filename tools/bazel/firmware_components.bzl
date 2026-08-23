"""Native firmware component metadata owned by platform component targets."""

load("@rules_cc//cc:find_cc_toolchain.bzl", "CC_TOOLCHAIN_ATTRS", "find_cpp_toolchain", "use_cc_toolchain")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _declared_include_root(root):
    path = str(root)
    return path not in ("", ".") and not path.startswith("bazel-out/")

FirmwareLibComponentInfo = provider(
    doc = "One native firmware component backed by multiple Bazel libraries.",
    fields = {
        "archives": "Complete ordered static archive closure imported by the component.",
        "component_name": "Native SDK component name.",
        "headers": "Complete public header closure required by native consumers.",
        "include_roots": "Declared public include roots required by native consumers.",
    },
)

FirmwareComponentsInfo = provider(
    doc = "Native firmware component archives reachable from a dependency graph.",
    fields = {"components": "Depset of component archive records."},
)

def _firmware_lib_component_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    if ctx.attr.validate_archive_abi and not cc_toolchain.nm_executable:
        fail("C++ toolchain for %s does not provide nm" % ctx.label)
    linked_archives = []
    seen_archives = {}
    include_roots = {}
    header_sets = []
    for dependency in ctx.attr.deps:
        cc_info = dependency[CcInfo]
        for linker_input in cc_info.linking_context.linker_inputs.to_list():
            for library in linker_input.libraries:
                archive = library.static_library
                if archive and archive.path not in seen_archives:
                    seen_archives[archive.path] = True
                    linked_archives.append(archive)

        compilation_context = cc_info.compilation_context
        header_sets.append(compilation_context.headers)
        for roots in [
            compilation_context.includes,
            compilation_context.quote_includes,
            compilation_context.system_includes,
            compilation_context.framework_includes,
        ]:
            for root in roots.to_list():
                if _declared_include_root(root):
                    include_roots[str(root)] = True

    if not linked_archives:
        fail("firmware library component %s does not contain a static archive" % ctx.label)

    validated_archives = []
    for index, archive in enumerate(linked_archives):
        if not ctx.attr.validate_archive_abi:
            validated_archives.append(archive)
            continue
        validated_archive = ctx.actions.declare_file(
            "%s_archive_abi/%d_%s" % (ctx.label.name, index, archive.basename),
        )
        ctx.actions.run(
            executable = ctx.executable._archive_abi_validator,
            arguments = [
                "--nm",
                cc_toolchain.nm_executable,
                "--owner",
                str(ctx.label),
                "--input",
                archive.path,
                "--output",
                validated_archive.path,
            ],
            inputs = depset(
                direct = [archive],
                transitive = [cc_toolchain.all_files],
            ),
            mnemonic = "FirmwareArchiveAbi",
            outputs = [validated_archive],
            progress_message = "Checking firmware archive ABI %{input}",
            use_default_shell_env = True,
        )
        validated_archives.append(validated_archive)

    return [
        DefaultInfo(files = depset(validated_archives, order = "topological")),
        FirmwareLibComponentInfo(
            archives = depset(validated_archives, order = "topological"),
            component_name = ctx.attr.component_name,
            headers = depset(transitive = header_sets),
            include_roots = sorted(include_roots),
        ),
    ]

_firmware_lib_component_attrs = {
    "_archive_abi_validator": attr.label(
        default = Label("//tools/bazel:firmware_archive_abi"),
        executable = True,
        cfg = "exec",
    ),
    "deps": attr.label_list(
        providers = [CcInfo],
    ),
    "component_name": attr.string(default = "h2_firmware_lib"),
    "validate_archive_abi": attr.bool(),
} | CC_TOOLCHAIN_ATTRS

_firmware_lib_component = rule(
    implementation = _firmware_lib_component_impl,
    attrs = _firmware_lib_component_attrs,
    fragments = ["cpp"],
    toolchains = use_cc_toolchain(),
)

def firmware_lib_component(name, **kwargs):
    """Declares a firmware component and validates archives in embedded configs."""
    _firmware_lib_component(
        name = name,
        validate_archive_abi = select({
            "@platforms//os:none": True,
            "//conditions:default": False,
        }),
        **kwargs
    )

def _firmware_components_aspect_impl(target, ctx):
    transitive = []
    if hasattr(ctx.rule.attr, "deps"):
        transitive = [
            dependency[FirmwareComponentsInfo].components
            for dependency in ctx.rule.attr.deps
            if FirmwareComponentsInfo in dependency
        ]

    direct = []
    if FirmwareLibComponentInfo in target:
        component = target[FirmwareLibComponentInfo]
        direct.append(struct(
            archives = component.archives,
            component_name = component.component_name,
            headers = component.headers,
            include_roots = component.include_roots,
            owner = str(target.label),
        ))

    return [FirmwareComponentsInfo(
        components = depset(direct = direct, transitive = transitive),
    )]

firmware_components_aspect = aspect(
    implementation = _firmware_components_aspect_impl,
    attr_aspects = ["deps"],
)

def _component_name(record):
    return record.component_name

def sort_firmware_components(components):
    """Returns component archive records ordered by native component name."""
    return sorted(components, key = _component_name)

def collect_firmware_components(graph):
    """Returns validated native component archive records from graph dependencies.

    Args:
      graph: Dependencies carrying ``FirmwareComponentsInfo`` providers.

    Returns:
      Stable component archive records ordered by component name.
    """
    by_name = {}
    by_owner = {}
    for dependency in graph:
        for component in dependency[FirmwareComponentsInfo].components.to_list():
            previous_owner = by_name.get(component.component_name)
            if previous_owner and previous_owner != component.owner:
                fail("firmware component name %s is owned by both %s and %s" % (
                    component.component_name,
                    previous_owner,
                    component.owner,
                ))
            by_name[component.component_name] = component.owner
            by_owner[component.owner] = component

    result = []
    for component in sort_firmware_components(by_owner.values()):
        component_name = component.component_name
        archives = component.archives.to_list()
        result.append(struct(
            archive = archives[0],
            component_name = component_name,
            headers = component.headers,
            include_roots = component.include_roots,
            generate_component = True,
        ))
        dependency_index = 0
        for archive in archives[1:]:
            result.append(struct(
                archive = archive,
                component_name = "%s_dependency_%d" % (component_name, dependency_index),
                headers = depset(),
                include_roots = [],
                generate_component = False,
            ))
            dependency_index += 1

    return result
