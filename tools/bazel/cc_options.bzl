"""Shared compiler warning policy for first-party C and C++ targets."""

def h2_gnu_only_copts(options):
    """Returns compiler options only for non-MSVC target configurations."""
    return select({
        "@gizos//tools/bazel/platforms:is_windows_x86_64": [],
        "//conditions:default": options,
    })

H2_C11_OPTS = select({
    "@gizos//tools/bazel/platforms:is_windows_x86_64": [
        "/experimental:c11atomics",
        "/std:c11",
    ],
    "//conditions:default": ["-std=c11"],
})

H2_GNU_C11_OPTS = h2_gnu_only_copts(["-std=c11"])

H2_CXX17_OPTS = select({
    "@gizos//tools/bazel/platforms:is_windows_x86_64": ["/std:c++17"],
    "//conditions:default": ["-std=c++17"],
})

H2_CXX11_OPTS = select({
    "@gizos//tools/bazel/platforms:is_windows_x86_64": ["/std:c++14"],
    "//conditions:default": ["-std=gnu++11"],
})

H2_CXX17_NO_EXCEPTIONS_RTTI_COPTS = select({
    "@gizos//tools/bazel/platforms:is_windows_x86_64": [
        "/EHs-c-",
        "/GR-",
        "/std:c++17",
    ],
    "//conditions:default": [
        "-fno-exceptions",
        "-fno-rtti",
        "-std=c++17",
    ],
})

H2_WARNING_COPTS = select({
    "@gizos//tools/bazel/platforms:is_windows_x86_64": [
        "/W4",
        "/WX",
    ],
    "//conditions:default": [
        "-Wall",
        "-Wextra",
        "-Werror",
    ],
})

H2_WARNING_COPTS_NO_ERROR = select({
    "@gizos//tools/bazel/platforms:is_windows_x86_64": ["/W4"],
    "//conditions:default": [
        "-Wall",
        "-Wextra",
    ],
})

H2_WALL_COPTS = select({
    "@gizos//tools/bazel/platforms:is_windows_x86_64": ["/W4"],
    "//conditions:default": ["-Wall"],
})

H2_PEDANTIC_WARNING_COPTS = select({
    "@gizos//tools/bazel/platforms:is_windows_x86_64": [
        "/W4",
        "/WX",
    ],
    "//conditions:default": [
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
    ],
})
