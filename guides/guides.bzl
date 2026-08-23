"""Rules for building the Guides site from declared repository inputs."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _api_reference_impl(ctx):
    output = ctx.actions.declare_directory(".generated")
    generator = ctx.attr.generator[DefaultInfo].files_to_run
    headers = []
    for dep in ctx.attr.deps:
        headers.extend(dep[CcInfo].compilation_context.headers.to_list())

    ctx.actions.run_shell(
        arguments = [
            generator.executable.path,
            output.path,
        ],
        command = '''
workspace="$PWD"
exec "$1" --repository-root="$workspace" --generated-root="$2"
''',
        env = {
            "BAZEL_BINDIR": ctx.bin_dir.path,
            "PATH": "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin",
        },
        inputs = depset(headers + [ctx.file.doxyfile]),
        mnemonic = "GuidesApiReference",
        outputs = [output],
        progress_message = "Generating Guides API reference",
        tools = [generator],
        use_default_shell_env = True,
    )
    return [DefaultInfo(files = depset([output]))]

api_reference = rule(
    implementation = _api_reference_impl,
    attrs = {
        "deps": attr.label_list(providers = [CcInfo]),
        "doxyfile": attr.label(allow_single_file = True, mandatory = True),
        "generator": attr.label(executable = True, cfg = "exec", mandatory = True),
    },
)
