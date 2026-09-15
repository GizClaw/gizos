"""Generate JieLi SDK task tables from target-owned Bazel policy rows.

Rows are ``name priority stack_words queue_words``. SDK tasks are separate
from portable tasks so the shared graph audit only checks declared PAL tasks.
Core affinity remains explicit in the SDK name, e.g. ``#C0btstack``.
"""

load(":native_component.bzl", "firmware_native_component")
load(":target_task_policy_codegen.bzl", "task_policy_audit")

def _row(row):
    columns = [part for part in row.split(" ") if part]
    if len(columns) != 4:
        fail("JieLi policy requires: name priority stack_words queue_words: %r" % row)
    name = columns[0]
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$#/-.:"
    if not name or any([char not in allowed for char in name.elems()]):
        fail("JieLi SDK table needs a literal task name: %r" % name)
    logical = name[3:] if name.startswith("#C0") or name.startswith("#C1") else name
    if not logical or len(logical) >= 32 or logical.startswith("$h2anon/"):
        fail("JieLi task name is too long or reserved: %r" % name)
    if not all([value.isdigit() for value in columns[1:]]):
        fail("JieLi policy budgets must be nonnegative integers: %r" % row)
    priority, stack, queue = [int(value) for value in columns[1:]]
    if priority > 255 or stack < 1 or stack > 0xffffffff or queue > 65535:
        fail("JieLi policy is outside SDK field bounds: %r" % row)
    return name, '{"%s", %du, %du, %du, 0}' % (name, priority, stack, queue)

def _source_impl(ctx):
    rows = []
    names = {}
    for row in ctx.attr.rows:
        name, initializer = _row(row)
        if name in names:
            fail("duplicate JieLi task policy: %s" % name)
        names[name] = True
        rows.append("    " + initializer + ",")
    _, fallback = _row("h2_default " + ctx.attr.default_policy)
    source = ctx.actions.declare_file(ctx.label.name + ".c")
    ctx.actions.write(source, "\n".join([
        "/* Generated from a target-owned Bazel policy. Do not edit. */",
        '#include "system/task.h"',
        "const struct task_info task_info_table[] = {",
    ] + rows + [
        "    {0, 0, 0, 0, 0},",
        "};",
        "const struct task_info h2_jieli_default_task_policy = " + fallback + ";",
        "",
    ]))
    return [DefaultInfo(files = depset([source]))]

_source = rule(
    implementation = _source_impl,
    attrs = {
        "rows": attr.string_list(),
        "default_policy": attr.string(mandatory = True),
    },
)

def jieli_target_task_policy(name, graph, policies, sdk_policies, default_policy, deps = []):
    """Own SDK task budgets and validate all reachable portable task names.

    default_policy has three columns: priority, stack words, queue words.
    It supplies dynamically named tasks; all statically declared tasks still
    require an explicit row and cannot silently inherit the fallback.
    """
    declared = {}
    for row in policies:
        task, _ = _row(row)
        # SDK affinity prefixes are not part of the PAL task's logical name.
        if task.startswith("#C0") or task.startswith("#C1"):
            task = task[3:]
        if task in declared:
            fail("duplicate JieLi portable policy: %s" % task)
        declared[task] = {}
    _source(
        name = name + "_source",
        rows = sdk_policies + policies,
        default_policy = default_policy,
    )
    task_policy_audit(
        name = name + "_audit",
        graph = graph,
        policies_json = json.encode(declared),
        policy_label = str(native.package_name()) + ":" + name,
    )
    firmware_native_component(
        name = name,
        srcs = [":" + name + "_source"],
        data = [":" + name + "_audit"],
        component_name = name,
        deps = deps,
    )
