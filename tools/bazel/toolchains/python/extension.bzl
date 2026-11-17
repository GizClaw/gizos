"""Register public rules_python local toolchains for the validated managed runtime."""

load("@gizos_python_host//:runtime.bzl", "RUNTIME")
load("@rules_python//python/local_toolchains:repos.bzl", "local_runtime_repo", "local_runtime_toolchains_repo")

def _managed_python_impl(_module_ctx):
    runtimes = ["gizos_managed_python_runtime"] if RUNTIME["enabled"] else []
    if runtimes:
        local_runtime_repo(
            name = runtimes[0],
            interpreter_target = "@gizos_python_host//:python",
            on_failure = "fail",
        )
    local_runtime_toolchains_repo(
        name = "gizos_managed_python_toolchains",
        runtimes = runtimes,
        exec_compatible_with = {repo: ["@platforms//os:linux", "@platforms//cpu:arm64"] for repo in runtimes},
        target_compatible_with = {repo: ["@platforms//os:linux", "@platforms//cpu:arm64"] for repo in runtimes},
        # Constrain even the default suites: Python 3.12 must not select 3.11.
        target_settings = {repo: ["@gizos_python_host//:managed_version"] for repo in runtimes},
    )

managed_python = module_extension(
    implementation = _managed_python_impl,
    arch_dependent = True,
    os_dependent = True,
)
