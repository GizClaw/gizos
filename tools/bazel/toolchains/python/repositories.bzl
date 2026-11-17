"""Select an optional, index-declared native Python without downloading a runtime."""

def select_python_package(document, requested):
    """Select the requested minor version; the manager owns its patch and path."""
    if type(document) != "dict" or type(document.get("packages")) != "list":
        return struct(package = None, error = "managed package index must contain a packages list")
    matches = [
        package
        for package in document["packages"]
        if type(package) == "dict" and
           package.get("kind") == "toolchain" and
           package.get("id") == "python" and
           package.get("guestRoot") == False and
           type(package.get("version")) == "string" and
           package["version"].startswith(requested + ".")
    ]
    if len(matches) > 1:
        matches = [package for package in matches if package.get("default") == True]
    if len(matches) != 1:
        return struct(package = None, error = "expected one managed Python %s package, found %d" % (requested, len(matches)))
    return struct(package = matches[0], error = "")

def _write_host(repository_ctx, metadata, interpreter):
    repository_ctx.file("runtime.json", json.encode_indent(metadata) + "\n")

    # A source file from this repository supplies the extension's configuration.
    # External files must be watched here: module_ctx cannot watch outside the workspace.
    repository_ctx.file("runtime.bzl", "RUNTIME = %r\n" % metadata)
    build = 'package(default_visibility = ["//visibility:public"])\nexports_files(["runtime.bzl", "runtime.json", "python"])\n'
    if interpreter:
        repository_ctx.symlink(interpreter, "python")
        versions = ["", metadata["requested"], metadata["version"]]
        build = 'load("@bazel_skylib//lib:selects.bzl", "selects")\n' + build
        settings = []
        for index, version in enumerate(versions):
            name = "version_%d" % index
            settings.append(":" + name)
            build += 'config_setting(name = %r, flag_values = {"@rules_python//python/config_settings:python_version": %r})\n' % (name, version)
        build += 'selects.config_setting_group(name = "managed_version", match_any = %r)\n' % settings
    else:
        # pip requires a file label, not an alias. Resolve the fallback only here,
        # keeping the unused hermetic runtime lazy when managed Python is enabled.
        repository_ctx.symlink(repository_ctx.path(Label(repository_ctx.attr.fallback)), "python")
    repository_ctx.file("BUILD.bazel", build)

def _managed_python_host_impl(repository_ctx):
    requested = repository_ctx.attr.python_version
    metadata = {"enabled": False, "requested": requested}
    interpreter = ""
    native_arm64 = repository_ctx.os.name.lower() == "linux" and repository_ctx.os.arch.lower() in ("aarch64", "arm64")
    packages = repository_ctx.getenv("GIZMOTH_PACKAGES", "") if native_arm64 else ""
    if packages:
        if not packages.startswith("/"):
            fail("GIZMOTH_PACKAGES must be an absolute managed package directory")
        root = repository_ctx.path(packages)
        index = root.get_child("index.json")
        repository_ctx.watch(index)
        selection = select_python_package(json.decode(repository_ctx.read(index)), requested)
        if selection.error:
            fail(selection.error)
        package = selection.package
        prefix = package.get("path", "")
        if type(prefix) != "string" or not prefix.startswith("/"):
            fail("managed Python package path must be absolute")
        prefix = repository_ctx.path(prefix)
        if not str(prefix.realpath).startswith(str(root.realpath) + "/"):
            fail("managed Python package must stay inside GIZMOTH_PACKAGES")
        interpreter = str(prefix.get_child("python").get_child("bin").get_child("python3"))
        repository_ctx.watch(interpreter)
        result = repository_ctx.execute([
            interpreter,
            "-I",
            "-B",
            repository_ctx.path(repository_ctx.attr._validator),
            "--version",
            package["version"],
            "--requested",
            requested,
            "--package-root",
            prefix,
        ], quiet = True, timeout = 60)
        if result.return_code:
            fail("managed Python version/ABI validation failed: %s" % result.stderr.strip())
        metadata = json.decode(result.stdout)
        metadata.update({"enabled": True, "index": str(index), "requested": requested})
        interpreter = metadata["interpreter"]
    _write_host(repository_ctx, metadata, interpreter)

managed_python_host = repository_rule(
    implementation = _managed_python_host_impl,
    attrs = {
        "fallback": attr.string(mandatory = True),
        "python_version": attr.string(mandatory = True),
        "_validator": attr.label(default = "//tools/bazel/toolchains/python:validate_runtime.py", allow_single_file = True),
    },
    environ = ["GIZMOTH_PACKAGES"],
)
