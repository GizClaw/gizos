"""Renders a firmware target's task policy component from its Bazel policy table.

A target's policy table is one row per task:

    "<task>  <priority>  <core>  <stack>  <region>"

or ``"<task>  default"`` for a task deliberately served by the target's
default policy. Columns are whitespace separated, so a table can be aligned
however it reads best. A task written with a trailing ``*`` is a prefix: it
covers every declared task name that starts with it.

The BK7258 CP unit has no per-core scheduling and today serves one policy to
every task, so it takes a default policy and no table.
"""

load(":tasks.bzl", "H2TasksInfo", "h2_tasks_aspect")

_ESP = struct(
    allocator = False,
    core = True,
    core_type = "h2_esp_task_core_t",
    cores = {"0": "H2_ESP_TASK_CORE_0", "1": "H2_ESP_TASK_CORE_1", "any": "H2_ESP_TASK_CORE_ANY"},
    header = "h2_esp_target_task_policy.h",
    install = "h2_esp_target_task_policy_install",
    platform_header = "h2_esp_platform_core.h",
    prefix = "h2_esp",
    regions = {"internal": "H2_ESP_TASK_STACK_INTERNAL", "psram": "H2_ESP_TASK_STACK_PSRAM"},
    # The route trie is looked up only when tasks are created, never with the
    # flash cache disabled, so it does not need internal RAM. esp_attr.h makes
    # the attribute empty when PSRAM BSS placement is not enabled. Host tests
    # compile the same source without ESP-IDF, where it is plain .bss.
    route_storage = "H2_TASK_POLICY_ROUTE_STORAGE ",
    route_storage_prelude = [
        "#if defined(ESP_PLATFORM)",
        "#include \"esp_attr.h\"",
        "#define H2_TASK_POLICY_ROUTE_STORAGE EXT_RAM_BSS_ATTR",
        "#else",
        "#define H2_TASK_POLICY_ROUTE_STORAGE",
        "#endif",
    ],
    sdk_name = False,
    unit = "esp",
)

_BK_REGIONS = {"default": "H2_BK_TASK_STACK_DEFAULT", "psram": "H2_BK_TASK_STACK_PSRAM"}

_BK_AP = struct(
    allocator = True,
    core = True,
    core_type = "uint32_t",
    cores = {"0": "0u", "1": "1u"},
    header = "h2_bk_target_task_policy.h",
    install = "h2_bk_target_task_policy_install",
    platform_header = "h2_bk_platform_core.h",
    prefix = "h2_bk",
    regions = _BK_REGIONS,
    route_storage = "",
    route_storage_prelude = [],
    sdk_name = True,
    unit = "ap",
)

_BK_CP = struct(
    allocator = False,
    core = False,
    core_type = "",
    cores = {},
    header = "h2_bk_target_task_policy.h",
    install = "h2_bk_target_task_policy_install",
    platform_header = "h2_bk_platform_core.h",
    prefix = "h2_bk",
    regions = _BK_REGIONS,
    route_storage = "",
    route_storage_prelude = [],
    sdk_name = True,
    unit = "cp",
)

_ALLOCATORS = {
    "default": "h2_bk_platform_default_allocator()",
    "psram": "h2_bk_platform_psram_allocator()",
    "sram": "h2_bk_platform_sram_allocator()",
}

_FLAVORS = {"esp": _ESP, "ap": _BK_AP, "cp": _BK_CP}

def _columns(row):
    return [column for column in row.split(" ") if column]

def _number(where, row, label, column):
    if not column.isdigit():
        fail("%s: %s must be a number in row %r" % (where, label, row))
    return int(column)

def _policy_columns(flavor):
    columns = ["priority"]
    if flavor.core:
        columns.append("core")
    return columns + ["stack", "region"]

def _parse_policy(where, flavor, row, columns):
    names = _policy_columns(flavor)
    if len(columns) != len(names):
        fail("%s: expected columns \"%s\" in row %r" % (where, "  ".join(names), row))
    values = {names[index]: columns[index] for index in range(len(names))}
    policy = {
        "priority": _number(where, row, "priority", values["priority"]),
        "min_stack_size": _number(where, row, "stack", values["stack"]),
    }
    if flavor.core:
        if values["core"] not in flavor.cores:
            fail("%s: core must be one of %s in row %r" % (
                where,
                ", ".join(sorted(flavor.cores)),
                row,
            ))
        policy["core"] = values["core"]
    if values["region"] not in flavor.regions:
        fail("%s: region must be one of %s in row %r" % (
            where,
            ", ".join(sorted(flavor.regions)),
            row,
        ))
    policy["stack_region"] = values["region"]
    return policy

def encode_policies(where, unit, policies):
    """Validates and encodes a target's task policy table.

    Args:
      where: Label-ish string naming the declaring target.
      unit: Execution unit the table belongs to: "esp", "ap" or "cp".
      policies: One row per task. See this file's module docstring.

    Returns:
      The encoded table, and the tasks left on the target default policy.
    """
    flavor = _FLAVORS[unit]
    routed = {}
    defaulted = []
    for row in policies:
        columns = _columns(row)
        if not columns:
            fail("%s: empty policy row" % where)
        task = columns[0]
        if task in routed or task in defaulted:
            fail("%s: task %s appears twice in the policy table" % (where, task))
        if len(columns) == 2 and columns[1] == "default":
            defaulted.append(task)
            continue
        routed[task] = _parse_policy(where, flavor, row, columns[1:])
    return json.encode(routed), defaulted

def encode_default_policy(where, unit, default_policy):
    """Validates and encodes the policy served to tasks without a row of their own.

    Args:
      where: Label-ish string naming the declaring target.
      unit: Execution unit the policy belongs to: "esp", "ap" or "cp".
      default_policy: The policy columns, without a task name.

    Returns:
      The encoded fallback policy, ready for a rule attribute.
    """
    flavor = _FLAVORS[unit]
    columns = _columns(default_policy)
    if not columns:
        fail("%s: %s needs a default policy: \"%s\"" % (
            where,
            unit,
            "  ".join(_policy_columns(flavor)),
        ))
    return json.encode(_parse_policy(where, flavor, default_policy, columns))

def _declared_tasks(label, graph):
    """Returns every task declared anywhere in the firmware's dependency graph."""
    tasks = []
    owners = {}
    for dependency in graph:
        if H2TasksInfo not in dependency:
            continue
        for record in dependency[H2TasksInfo].tasks.to_list():
            owner = owners.get(record.name)
            if owner == record.owner:
                continue
            if owner != None:
                fail("%s: task %s is declared by both %s and %s" % (
                    label,
                    record.name,
                    owner,
                    record.owner,
                ))
            owners[record.name] = record.owner
            tasks.append(record)
    return tasks, owners

def _covering_prefix(prefixes, name):
    covering = None
    for prefix in prefixes:
        if name.startswith(prefix) and (covering == None or len(prefix) > len(covering)):
            covering = prefix
    return covering

def _routed_tasks(policies, default_tasks):
    """Returns the routes the table asks for, ordered as the table lists them."""
    routed = []
    for name in sorted(policies):
        routed.append(struct(name = name, policy = policies[name]))
    return routed, list(default_tasks)

def _audit(label, graph, policies, default_tasks):
    """Fails when the table and the image's declared tasks disagree."""
    tasks, owners = _declared_tasks(label, graph)
    prefixes = [
        name[:-1]
        for name in policies.keys() + default_tasks
        if name.endswith("*")
    ]
    for name in policies.keys() + default_tasks:
        if name.endswith("*"):
            if not [task for task in tasks if task.name.startswith(name[:-1])]:
                fail("%s: the policy table has a row for %s, which covers no declared task" % (
                    label,
                    name,
                ))
        elif name not in owners:
            fail("%s: the policy table has a row for %s, which nothing in the image declares" % (
                label,
                name,
            ))
    unconfigured = [
        task
        for task in tasks
        if task.name not in policies and
           task.name not in default_tasks and
           _covering_prefix(prefixes, task.name) == None
    ]
    if unconfigured:
        fail("\n".join([
            "%s: %d task name(s) reachable from this target have no policy." % (
                label,
                len(unconfigured),
            ),
            "Add a row for each, or a \"<task>  default\" row to serve it the",
            "target default. Missing tasks, with the module that declares them:",
        ] + [
            "  %s  (%s)" % (task.name, task.owner)
            for task in unconfigured
        ]))
    return [task.name for task in tasks]

def _initializer(flavor, policy, indent):
    pad = " " * indent
    lines = []
    if flavor.sdk_name:
        lines.append(pad + ".sdk_name = NULL,")
    if flavor.core:
        lines.append(pad + ".core = %s," % flavor.cores[policy["core"]])
    lines.append(pad + ".priority = %du," % policy["priority"])
    lines.append(pad + ".min_stack_size = %du," % policy["min_stack_size"])
    lines.append(pad + ".stack_region = %s," % flavor.regions[policy["stack_region"]])
    return lines

def _policy_key(policy):
    return "|".join(["%s=%s" % (field, policy[field]) for field in sorted(policy)])

def _literal(name):
    escaped = name.replace("\\", "\\\\").replace("\"", "\\\"")
    return "\"%s\"" % escaped

def _route(name):
    if name.endswith("*"):
        return _literal(name[:-1]), "H2_TRIE_ROUTE_PREFIX"
    return _literal(name), "H2_TRIE_ROUTE_EXACT"

def _install_log(flavor, target_directory, stage):
    if stage:
        return [
            "    printf(\"H2_PAL_TASK_POLICY_FAIL unit=%s \"" % flavor.unit,
            "           \"target=%s \"" % target_directory,
            "           \"stage=%s reason=%%d\\n\"," % stage,
            "           rc);",
        ]
    return [
        "    printf(\"H2_PAL_TASK_POLICY_READY unit=%s \"" % flavor.unit,
        "           \"target=%s\\n\");" % target_directory,
    ]

def render_policy_source(
        label,
        unit,
        target_directory,
        tasks,
        unrouted,
        default_policy,
        allocator):
    """Returns the C source implementing one target's task policy component.

    Args:
      label: Declaring Bazel label, recorded in the generated banner.
      unit: Execution unit: "esp", "ap" or "cp".
      target_directory: Repository-relative target directory, logged at install.
      tasks: Routed task records carrying a complete policy.
      unrouted: Task names that intentionally fall through to the default.
      default_policy: Policy served to every unrouted task name.
      allocator: Memory the unit allocates task stacks from, if it takes one.

    Returns:
      The rendered C source text.
    """
    flavor = _FLAVORS[unit]
    policy_type = "%s_task_policy_t" % flavor.prefix
    lines = [
        "/* Generated from %s. Do not edit. */" % label,
        "",
        "#include \"%s\"" % flavor.header,
        "",
        "#include \"%s\"" % flavor.platform_header,
    ]
    if tasks:
        lines.append("#include \"h2_trie.h\"")
        lines.extend(flavor.route_storage_prelude)
    lines.extend(["", "#include <stdio.h>", ""])
    if unrouted:
        lines.append("/* Served by the target default policy: %s. */" % ", ".join(unrouted))
        lines.append("")

    shared = {}
    for task in tasks:
        key = _policy_key(task.policy)
        if key not in shared:
            symbol = "s_policy_%d" % len(shared)
            shared[key] = symbol
            lines.append("static const %s %s = {" % (policy_type, symbol))
            lines.extend(_initializer(flavor, task.policy, 4))
            lines.append("};")

    if tasks:
        lines.extend([
            "",
            "static h2_pal_result_t handle_policy(const void *user,",
            "                                     const h2_trie_match_t *match,",
            "                                     void *response) {",
            "  (void)match;",
            "  if (user == NULL || response == NULL) {",
            "    return H2_PAL_ERR_INVALID_ARG;",
            "  }",
            "  *(%s *)response = *(const %s *)user;" % (policy_type, policy_type),
            "  return H2_PAL_OK;",
            "}",
            "",
            "static const h2_trie_route_t s_routes[] = {",
        ])
        capacity = ["1u"]
        for task in tasks:
            key, mode = _route(task.name)
            lines.append("    {%s, %s, handle_policy," % (key, mode))
            lines.append("     &%s}," % shared[_policy_key(task.policy)])
            capacity.append("H2_TRIE_LITERAL_NODE_COUNT(%s)" % key)
        lines.extend([
            "};",
            "",
            "enum {",
            "  ROUTE_NODE_CAPACITY = " + ("\n" + " " * 24 + "+ ").join(capacity) + ",",
            "};",
            "",
            "%sstatic h2_trie_node_t s_route_nodes[ROUTE_NODE_CAPACITY];" % flavor.route_storage,
            "static h2_trie_t s_router;",
            "",
            "static h2_pal_result_t resolve_policy(void *user, const char *name,",
            "                                      %s *out_policy) {" % policy_type,
            "  (void)user;",
            "  /* Reject invalid inputs before route or fallback dispatch. */",
            "  if (name == NULL || name[0] == '\\0' || out_policy == NULL) {",
            "    return H2_PAL_ERR_NOT_FOUND;",
            "  }",
            "  return h2_trie_handle(&s_router, name, out_policy);",
            "}",
            "",
            "static h2_pal_result_t handle_default_policy(const void *user,",
            "                                            const h2_trie_match_t *match,",
            "                                            void *response) {",
            "  (void)user;",
            "  (void)match;",
            "  if (response == NULL) {",
            "    return H2_PAL_ERR_INVALID_ARG;",
            "  }",
            "  *(%s *)response = (%s){" % (policy_type, policy_type),
        ] + _initializer(flavor, default_policy, 6) + [
            "  };",
            "  return H2_PAL_OK;",
            "}",
            "",
        ])
    else:
        lines.extend([
            "static h2_pal_result_t resolve_policy(void *user, const char *name,",
            "                                      %s *out_policy) {" % policy_type,
            "  (void)user;",
            "  /* Reject invalid inputs the way a routed resolver does. */",
            "  if (name == NULL || name[0] == '\\0' || out_policy == NULL) {",
            "    return H2_PAL_ERR_NOT_FOUND;",
            "  }",
            "  *out_policy = (%s){" % policy_type,
        ] + _initializer(flavor, default_policy, 6) + [
            "  };",
            "  return H2_PAL_OK;",
            "}",
            "",
        ])

    lines.append("h2_pal_result_t %s(void) {" % flavor.install)
    if tasks:
        lines.extend([
            "  h2_pal_result_t rc =",
            "      h2_trie_build(&s_router, s_route_nodes, ROUTE_NODE_CAPACITY, s_routes,",
            "                    sizeof(s_routes) / sizeof(s_routes[0]));",
            "  if (rc != H2_PAL_OK) {",
        ] + _install_log(flavor, target_directory, "routes") + [
            "    return rc;",
            "  }",
            "  rc = h2_trie_set_fallback(&s_router, handle_default_policy, NULL);",
            "  if (rc != H2_PAL_OK) {",
            "    return rc;",
            "  }",
        ])
    config = [
        "  static const %s_task_policy_config_t config = {" % flavor.prefix,
        "      .resolver = resolve_policy,",
        "      .resolver_user = NULL,",
    ]
    if flavor.allocator:
        # The allocator comes from a call, so this config cannot be static.
        config[0] = "  const %s_task_policy_config_t config = {" % flavor.prefix
        config.append("      .task_allocator = %s," % _ALLOCATORS[allocator])
    lines.extend(config)
    lines.append("  };")
    lines.append("  %s%s_platform_task_configure(&config);" % (
        "rc = " if tasks else "h2_pal_result_t rc = ",
        flavor.prefix,
    ))
    lines.extend([
        "  if (rc == H2_PAL_OK) {",
    ] + _install_log(flavor, target_directory, "") + [
        "  } else {",
    ] + _install_log(flavor, target_directory, "configure") + [
        "  }",
        "  return rc;",
        "}",
        "",
    ])
    return "\n".join(lines)

def render_policy_test(label, unit, tasks, default_policy, allocator):
    """Returns the host test asserting one target's generated policy.

    Args:
      label: Declaring Bazel label, recorded in the generated banner.
      unit: Execution unit: "esp", "ap" or "cp".
      tasks: Routed task records carrying a complete policy.
      default_policy: Policy served to every unrouted task name.
      allocator: Memory the unit allocates task stacks from, if it takes one.

    Returns:
      The rendered C test source text.
    """
    flavor = _FLAVORS[unit]
    policy_type = "%s_task_policy_t" % flavor.prefix
    arguments = ["uint32_t priority"]
    if flavor.core:
        arguments.append("%s core" % flavor.core_type)
    arguments.append("uint32_t min_stack_size")
    arguments.append("%s_task_stack_region_t region" % flavor.prefix)

    def assertion(name, policy):
        values = ["%du" % policy["priority"]]
        if flavor.core:
            values.append(flavor.cores[policy["core"]])
        values.append("%du" % policy["min_stack_size"])
        values.append(flavor.regions[policy["stack_region"]])
        return "  assert_policy(%s, %s);" % (name, ", ".join(values))

    lines = [
        "/* Generated from %s. Do not edit. */" % label,
        "",
        "#include \"%s\"" % flavor.header,
        "",
        "#include \"%s\"" % flavor.platform_header,
        "",
        "#include <assert.h>",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "static %s_task_policy_config_t s_config;" % flavor.prefix,
        "static h2_pal_result_t s_configure_result = H2_PAL_OK;",
        "",
    ] + ([
        "static h2_pal_mem_api_t s_allocator;",
        "",
        "h2_pal_mem_api_t *%s(void) { return &s_allocator; }" % _ALLOCATORS[allocator][:-2],
        "",
    ] if flavor.allocator else []) + [
        "h2_pal_result_t",
        "%s_platform_task_configure(const %s_task_policy_config_t *config) {" % (
            flavor.prefix,
            flavor.prefix,
        ),
        "  s_config = *config;",
        "  return s_configure_result;",
        "}",
        "",
        "static h2_pal_result_t get_policy(const char *name, %s *out_policy) {" % policy_type,
        "  return s_config.resolver(s_config.resolver_user, name, out_policy);",
        "}",
        "",
        "static void assert_policy(const char *name, %s) {" % ",\n                          ".join(arguments),
        "  %s policy = {0};" % policy_type,
        "  assert(get_policy(name, &policy) == H2_PAL_OK);",
    ]
    if flavor.sdk_name:
        lines.append("  assert(policy.sdk_name == NULL);")
    lines.append("  assert(policy.priority == priority);")
    if flavor.core:
        lines.append("  assert(policy.core == core);")
    lines.extend([
        "  assert(policy.min_stack_size == min_stack_size);",
        "  assert(policy.stack_region == region);",
        "}",
        "",
        "int main(void) {",
        "  %s policy = {0};" % policy_type,
        "  assert(%s() == H2_PAL_OK);" % flavor.install,
    ])
    if flavor.allocator:
        lines.append("  assert(s_config.task_allocator == &s_allocator);")
    for task in tasks:
        name = task.name[:-1] + "any-task" if task.name.endswith("*") else task.name
        lines.append(assertion(_literal(name), task.policy))
    lines.append(assertion("\"dynamic-default\"", default_policy))
    lines.extend([
        "  assert(get_policy(\"\", &policy) == H2_PAL_ERR_NOT_FOUND);",
        "  assert(get_policy(NULL, &policy) == H2_PAL_ERR_NOT_FOUND);",
        "  assert(get_policy(\"unknown\", NULL) == H2_PAL_ERR_NOT_FOUND);",
    ])
    lines.extend([
        "  s_configure_result = H2_PAL_ERR_INVALID_STATE;",
        "  assert(%s() == H2_PAL_ERR_INVALID_STATE);" % flavor.install,
        "  return 0;",
        "}",
        "",
    ])
    return "\n".join(lines)

def _render_policy_header(flavor):
    guard = flavor.header.upper().replace(".", "_")
    return "\n".join([
        "/* Generated task policy installation API. Do not edit. */",
        "#ifndef " + guard,
        "#define " + guard,
        "",
        '#include "h2/pal/core/h2_pal_errors.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
        "h2_pal_result_t %s(void);" % flavor.install,
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
        "#endif",
        "",
    ])

def _render_policy_cmake(flavor):
    component = flavor.prefix + "_target_task_policy"
    unit = " " + flavor.unit if flavor.unit != "esp" else ""
    register = "idf" if flavor.unit == "esp" else "armino"
    requires = ["h2_pal_core"]
    if flavor.unit != "cp":
        requires.insert(0, "h2_firmware_lib")
    if flavor.unit != "esp":
        requires.append("bk_rtos")
    return "\n".join([
        "# Generated task policy component. Do not edit.",
        'include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")',
        "h2_bazel_native_component_sources(H2_TARGET_TASK_POLICY_SOURCES %s%s)" % (component, unit),
        "",
        "%s_component_register(" % register,
        "    SRCS ${H2_TARGET_TASK_POLICY_SOURCES}",
        '    INCLUDE_DIRS "."',
        "    REQUIRES %s)" % " ".join(requires),
        "",
    ])

def _task_policy_codegen_impl(ctx):
    label = str(ctx.label)
    tasks, unrouted = _routed_tasks(
        json.decode(ctx.attr.policies_json),
        ctx.attr.default_tasks,
    )
    default_policy = json.decode(ctx.attr.default_policy_json)
    flavor = _FLAVORS[ctx.attr.unit]
    directory = ctx.attr.source_name.rsplit("/", 1)[0]
    header = ctx.actions.declare_file(directory + "/" + flavor.header)
    cmake = ctx.actions.declare_file(directory + "/CMakeLists.txt")
    ctx.actions.write(header, _render_policy_header(flavor))
    ctx.actions.write(cmake, _render_policy_cmake(flavor))
    source = ctx.actions.declare_file(ctx.attr.source_name)
    test_source = ctx.actions.declare_file(ctx.attr.test_source_name)
    ctx.actions.write(
        source,
        render_policy_source(
            label,
            ctx.attr.unit,
            ctx.attr.target_directory,
            tasks,
            unrouted,
            default_policy,
            ctx.attr.allocator,
        ),
    )
    ctx.actions.write(
        test_source,
        render_policy_test(label, ctx.attr.unit, tasks, default_policy, ctx.attr.allocator),
    )
    return [
        DefaultInfo(files = depset([source, header, cmake, test_source])),
        OutputGroupInfo(
            source = depset([source]),
            header = depset([header]),
            cmake = depset([cmake]),
            test_source = depset([test_source]),
        ),
    ]

task_policy_codegen = rule(
    implementation = _task_policy_codegen_impl,
    attrs = {
        "allocator": attr.string(
            doc = "Memory this unit allocates task stacks from.",
            values = ["", "default", "psram", "sram"],
        ),
        "default_policy_json": attr.string(
            doc = "Encoded policy served to task names without a row of their own.",
            mandatory = True,
        ),
        "default_tasks": attr.string_list(
            doc = "Task names deliberately served by the target default policy.",
        ),
        "policies_json": attr.string(
            doc = "Encoded per-task policies assigned by this target.",
            mandatory = True,
        ),
        "source_name": attr.string(
            doc = "Generated component source file name.",
            mandatory = True,
        ),
        "target_directory": attr.string(
            doc = "Repository-relative target directory reported in install logs.",
            mandatory = True,
        ),
        "test_source_name": attr.string(
            doc = "Generated host test source file name.",
            mandatory = True,
        ),
        "unit": attr.string(
            doc = "Execution unit this policy configures.",
            mandatory = True,
            values = ["esp", "ap", "cp"],
        ),
    },
    doc = "Generates one target's task policy component and its host test.",
)

def _task_policy_audit_impl(ctx):
    declared = _audit(
        str(ctx.attr.policy_label),
        ctx.attr.graph,
        json.decode(ctx.attr.policies_json),
        ctx.attr.default_tasks,
    )
    stamp = ctx.actions.declare_file(ctx.label.name + ".tasks")
    ctx.actions.write(stamp, "\n".join(sorted(declared)) + "\n")
    return [DefaultInfo(files = depset([stamp]))]

task_policy_audit = rule(
    implementation = _task_policy_audit_impl,
    attrs = {
        "default_tasks": attr.string_list(
            doc = "Task names deliberately served by the target default policy.",
        ),
        "graph": attr.label_list(
            aspects = [h2_tasks_aspect],
            doc = "The firmware dependency graph whose task declarations to audit.",
        ),
        "policies_json": attr.string(
            doc = "Encoded per-task policies assigned by this target.",
            mandatory = True,
        ),
        "policy_label": attr.string(
            doc = "Policy target named in audit failures.",
            mandatory = True,
        ),
    },
    doc = "Fails the build when a target's policy table and its image's tasks disagree.",
)
