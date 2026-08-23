"""Build-time validation and C++ generation for Desktop layout JSON."""

def _layout_config_impl(ctx):
    output = ctx.actions.declare_file(ctx.label.name + "/layout_config.h")
    arguments = ctx.actions.args()
    arguments.add("--input", ctx.file.src.path)
    arguments.add("--output", output.path)
    arguments.add("--app-name", ctx.attr.app_name)
    arguments.add("--repo-root", ".")
    ctx.actions.run(
        arguments = [arguments],
        executable = ctx.executable._generator,
        inputs = depset(
            direct = [ctx.file.src],
            transitive = [target[DefaultInfo].files for target in ctx.attr.mount_inputs],
        ),
        mnemonic = "H2DesktopLayout",
        progress_message = "Validating Desktop layout %{label}",
        outputs = [output],
    )
    return [DefaultInfo(files = depset([output]))]

h2_desktop_layout_config = rule(
    implementation = _layout_config_impl,
    attrs = {
        "app_name": attr.string(mandatory = True),
        "mount_inputs": attr.label_list(allow_files = True),
        "src": attr.label(allow_single_file = [".json"], mandatory = True),
        "_generator": attr.label(
            cfg = "exec",
            default = Label("//tools/bazel/desktop_layout:generate_layout"),
            executable = True,
        ),
    },
)
