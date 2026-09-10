"""Runtime task names declared by one module.

A module states the names of the tasks it starts and puts that declaration in
its own dependency list, so any firmware image that links the module carries
its tasks along. Scheduling policy stays with the firmware target, which
weighs every task in its own image against its board and RTOS; a target that
leaves a reachable task unconfigured is told at analysis time which module
brought it in.

This is a build-graph declaration only: the module names its tasks however it
likes in its own sources, and nothing here reaches the compiler.
"""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

H2TasksInfo = provider(
    doc = "Runtime task names one module starts.",
    fields = {
        "tasks": "Depset of runtime task name records reachable from this target.",
    },
)

_TASK_ATTRIBUTES = [
    "deps",
    "implementation_deps",
    "exports",
    "runtime_deps",
    "srcs",
    "hdrs",
    "textual_hdrs",
    "data",
    "graph",
    "task_policy",
]

def _collect(attributes):
    transitive = []
    for name in _TASK_ATTRIBUTES:
        if not hasattr(attributes, name):
            continue
        dependencies = getattr(attributes, name)
        if type(dependencies) != "list":
            dependencies = [dependencies]
        for dependency in dependencies:
            if type(dependency) == "Target" and H2TasksInfo in dependency:
                transitive.append(dependency[H2TasksInfo].tasks)
    return transitive

def _h2_tasks_aspect_impl(target, ctx):
    if H2TasksInfo in target:
        # The declaration itself already carries what it and its deps own.
        return []
    return [H2TasksInfo(tasks = depset(transitive = _collect(ctx.rule.attr)))]

h2_tasks_aspect = aspect(
    implementation = _h2_tasks_aspect_impl,
    attr_aspects = _TASK_ATTRIBUTES,
    doc = "Collects the task declarations reachable from a firmware graph.",
)

def _h2_tasks_impl(ctx):
    seen = {}
    records = []
    for task in ctx.attr.tasks:
        if not task:
            fail("%s: task names cannot be empty" % ctx.label)
        if task in seen:
            fail("%s: task %s is declared twice" % (ctx.label, task))
        seen[task] = True
        records.append(struct(name = task, owner = str(ctx.label)))
    return [
        CcInfo(),
        H2TasksInfo(tasks = depset(
            direct = records,
            transitive = _collect(ctx.attr),
        )),
    ]

h2_tasks = rule(
    implementation = _h2_tasks_impl,
    attrs = {
        "deps": attr.label_list(
            aspects = [h2_tasks_aspect],
            doc = "Other task declarations this one carries along.",
        ),
        "tasks": attr.string_list(
            doc = "Runtime task names this module starts.",
            mandatory = True,
        ),
    },
    doc = "Declares the runtime task names one module starts.",
)
