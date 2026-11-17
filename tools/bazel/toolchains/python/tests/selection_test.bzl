"""Exercise selection from the manager's real package-index schema."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load("//tools/bazel/toolchains/python:repositories.bzl", "select_python_package")

def _python(version, default = False, guest_root = False):
    return {
        "kind": "toolchain",
        "id": "python",
        "version": version,
        "default": default,
        "guestRoot": guest_root,
        "path": "/managed/toolchains/python/" + version,
    }

def _selection_test_impl(ctx):
    env = unittest.begin(ctx)
    selected = select_python_package({"packages": [_python("3.11.18"), _python("3.12.16", default = True)]}, "3.11")
    asserts.equals(env, "", selected.error)
    asserts.equals(env, "3.11.18", selected.package["version"])
    selected = select_python_package({"packages": [_python("3.11.17"), _python("3.11.18", default = True)]}, "3.11")
    asserts.equals(env, "3.11.18", selected.package["version"])
    for document in [
        {},
        {"packages": [_python("3.12.16", default = True)]},
        {"packages": [_python("3.11.18", guest_root = True)]},
        {"packages": [_python("3.11.17"), _python("3.11.18")]},
    ]:
        result = select_python_package(document, "3.11")
        asserts.true(env, bool(result.error))
        asserts.equals(env, None, result.package)
    return unittest.end(env)

managed_python_selection_test = unittest.make(_selection_test_impl)
