"""Bzlmod repositories for pinned upstream archives and host SDK inputs."""

_FFMPEG_CONFIGURE_ADAPTER = """#!/bin/sh

set -eu

configure="$1"
shift

configure_cc=$CC
configure_cxx=$CXX
configure_ld=$CC
configure_ar=$AR
if [ "${H2_FFMPEG_CONFIG_TARGET:-}" != linux ]; then
  tool_dir=.h2-ffmpeg-tools
  mkdir -p "$tool_dir"
  ln -sf "$CC" "$tool_dir/wrapped_clang"
  ln -sf "$CXX" "$tool_dir/wrapped_clang_pp"
  ln -sf "$AR" "$tool_dir/libtool"
  configure_cc=$tool_dir/wrapped_clang
  configure_cxx=$tool_dir/wrapped_clang_pp
  configure_ld=$tool_dir/wrapped_clang
  configure_ar=$tool_dir/libtool

  # rules_foreign_cc passes BUILD_TMPDIR/ffmpeg_foreign as an absolute sandbox
  # prefix. FFmpeg records its configure arguments in every runtime dylib, so
  # keep both the tool names and the prefix relative to the action's build root.
  # configure runs from BUILD_TMPDIR, making this literal the same install
  # directory that rules_foreign_cc copies after `make install`.
  argument_count=$#
  while [ "$argument_count" -gt 0 ]; do
    argument="$1"
    shift
    argument_count=$((argument_count - 1))
    case "$argument" in
      --prefix=*) argument="--prefix=ffmpeg_foreign" ;;
    esac
    set -- "$@" "$argument"
  done

  CFLAGS="${CFLAGS:-} -ffile-prefix-map=${EXT_BUILD_ROOT:?}=."
  CXXFLAGS="${CXXFLAGS:-} -ffile-prefix-map=${EXT_BUILD_ROOT:?}=."
  export CFLAGS CXXFLAGS
fi

"$configure" \\
  --cc="$configure_cc" \\
  --cxx="$configure_cxx" \\
  --ld="$configure_ld" \\
  --ar="$configure_ar" \\
  --ranlib="$RANLIB" \\
  "$@"

if [ "${H2_FFMPEG_CONFIG_TARGET:-}" = linux ]; then
  for feature in HAVE_SYSCTL HAVE_SYSCTLBYNAME; do
    grep -Eq "^#define $feature [01]$" config.h || {
      printf 'error: FFmpeg config.h does not define %s\\n' "$feature" >&2
      exit 1
    }
    sed "s/^#define $feature [01]$/#define $feature 0/" config.h \\
      > config.h.h2
    mv config.h.h2 config.h
  done
fi
"""

_VENDOR_METADATA_FILENAMES = {
    ".git": True,
    "BUILD": True,
    "BUILD.bazel": True,
    "MODULE.bazel": True,
    "REPO.bazel": True,
    "WORKSPACE": True,
    "WORKSPACE.bazel": True,
}

def _source_files(source_root):
    files = []
    directories = [struct(path = source_root, relative = "")]
    for _depth in range(256):
        if not directories:
            break
        next_directories = []
        for current in directories:
            entries = {}
            for entry in current.path.readdir(watch = "no"):
                entries[entry.basename] = entry
            for name in sorted(entries.keys()):
                if name in _VENDOR_METADATA_FILENAMES:
                    continue
                entry = entries[name]
                relative = name if not current.relative else current.relative + "/" + name
                if entry.is_dir:
                    next_directories.append(struct(path = entry, relative = relative))
                else:
                    files.append(struct(path = entry, relative = relative))
        directories = next_directories
    if directories:
        fail("vendor source tree exceeds the supported 256-directory depth")
    return files

def _remove_upstream_repository_metadata(repository_ctx, source_root):
    directories = [source_root]
    for _depth in range(256):
        if not directories:
            break
        next_directories = []
        for current in directories:
            for entry in current.readdir(watch = "no"):
                if entry.basename in _VENDOR_METADATA_FILENAMES:
                    if not repository_ctx.delete(entry):
                        fail("failed to remove upstream repository metadata %s" % entry)
                elif entry.is_dir:
                    next_directories.append(entry)
        directories = next_directories
    if directories:
        fail("vendor archive exceeds the supported 256-directory depth")

def _install_vendor_metadata(repository_ctx):
    for patch in repository_ctx.attr.patches:
        repository_ctx.patch(patch)
    for source, destination in repository_ctx.attr.overlay_files.items():
        if (not destination or destination.startswith("/") or
            ".." in destination.split("/")):
            fail("invalid vendor repository overlay destination: %s" % destination)
        repository_ctx.file(destination, repository_ctx.read(source))
    repository_ctx.file(
        "BUILD.bazel",
        repository_ctx.read(repository_ctx.attr.build_file),
    )
    if repository_ctx.attr.ffmpeg_configure_adapter:
        repository_ctx.file(
            "ffmpeg_configure.sh",
            _FFMPEG_CONFIGURE_ADAPTER,
            executable = True,
        )

def _archive_repository_impl(repository_ctx):
    if not repository_ctx.attr.urls:
        fail("vendor archive requires at least one immutable URL")
    if len(repository_ctx.attr.sha256) != 64:
        fail("vendor archive requires a 64-character SHA-256 digest")
    if not repository_ctx.attr.strip_prefix:
        fail("vendor archive requires its extracted root as strip_prefix")
    repository_ctx.download_and_extract(
        url = repository_ctx.attr.urls,
        output = "src",
        sha256 = repository_ctx.attr.sha256,
        stripPrefix = repository_ctx.attr.strip_prefix,
        type = "tar.gz",
    )
    nested_paths = sorted(repository_ctx.attr.nested_archive_urls.keys())
    if (nested_paths != sorted(repository_ctx.attr.nested_archive_sha256.keys()) or
        nested_paths != sorted(repository_ctx.attr.nested_archive_strip_prefix.keys())):
        fail("nested vendor archive URL, SHA-256, and strip-prefix paths must match")
    for nested_path in nested_paths:
        if (not nested_path or nested_path.startswith("/") or
            ".." in nested_path.split("/")):
            fail("invalid nested vendor archive destination: %s" % nested_path)
        nested_sha256 = repository_ctx.attr.nested_archive_sha256[nested_path]
        if len(nested_sha256) != 64:
            fail("nested vendor archive %s requires a 64-character SHA-256 digest" % nested_path)
        nested_strip_prefix = repository_ctx.attr.nested_archive_strip_prefix[nested_path]
        if not nested_strip_prefix:
            fail("nested vendor archive %s requires strip_prefix" % nested_path)
        repository_ctx.download_and_extract(
            url = repository_ctx.attr.nested_archive_urls[nested_path],
            output = "src/" + nested_path,
            sha256 = nested_sha256,
            stripPrefix = nested_strip_prefix,
            type = "tar.gz",
        )
    _remove_upstream_repository_metadata(repository_ctx, repository_ctx.path("src"))
    _install_vendor_metadata(repository_ctx)

_archive_repository = repository_rule(
    implementation = _archive_repository_impl,
    attrs = {
        "build_file": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "ffmpeg_configure_adapter": attr.bool(default = False),
        "nested_archive_sha256": attr.string_dict(),
        "nested_archive_strip_prefix": attr.string_dict(),
        "nested_archive_urls": attr.string_dict(),
        "overlay_files": attr.label_keyed_string_dict(allow_files = True),
        "patches": attr.label_list(allow_files = True),
        "sha256": attr.string(mandatory = True),
        "strip_prefix": attr.string(mandatory = True),
        "urls": attr.string_list(mandatory = True),
    },
)

def _local_vendor_repository_impl(repository_ctx):
    workspace_root = repository_ctx.path(repository_ctx.attr.workspace_file).dirname
    source_root = workspace_root.get_child(repository_ctx.attr.path)
    if not source_root.exists:
        fail("local vendor source %s is missing" % repository_ctx.attr.path)
    repository_ctx.watch_tree(source_root)
    discovered = _source_files(source_root)
    if not discovered:
        fail("local vendor source %s is empty" % repository_ctx.attr.path)

    for source in discovered:
        destination = "src/" + source.relative
        repository_ctx.symlink(source.path, destination)
    _install_vendor_metadata(repository_ctx)

_local_vendor_repository = repository_rule(
    implementation = _local_vendor_repository_impl,
    attrs = {
        "build_file": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "ffmpeg_configure_adapter": attr.bool(default = False),
        "overlay_files": attr.label_keyed_string_dict(allow_files = True),
        "path": attr.string(mandatory = True),
        "patches": attr.label_list(allow_files = True),
        "workspace_file": attr.label(
            allow_single_file = True,
            default = Label("//:MODULE.bazel"),
        ),
    },
    local = True,
)

def _fdk_aac_repository_impl(repository_ctx):
    repository_ctx.download_and_extract(
        url = "https://codeload.github.com/mstorsjo/fdk-aac/tar.gz/v2.0.1",
        sha256 = "a4142815d8d52d0e798212a5adea54ecf42bcd4eec8092b37a8cb615ace91dc6",
        stripPrefix = "fdk-aac-2.0.1",
        type = "tar.gz",
    )
    repository_ctx.file(
        "BUILD.bazel",
        repository_ctx.read(repository_ctx.attr.build_file),
    )
    for source, destination in repository_ctx.attr.overlay_files.items():
        if (not destination or destination.startswith("/") or
            ".." in destination.split("/")):
            fail("invalid FDK-AAC overlay destination: %s" % destination)
        repository_ctx.file(destination, repository_ctx.read(source))
    for patch in repository_ctx.attr.patches:
        repository_ctx.patch(patch, strip = 1)

_fdk_aac_repository = repository_rule(
    implementation = _fdk_aac_repository_impl,
    attrs = {
        "build_file": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "overlay_files": attr.label_keyed_string_dict(allow_files = True),
        "patches": attr.label_list(allow_files = True),
    },
)

def _first_header_root(repository_ctx, candidates, marker):
    for candidate in candidates:
        path = repository_ctx.path(candidate)
        if path.get_child(marker).exists:
            return path
    fail("system headers not found for %s" % marker)

def _symlink_header_tree(repository_ctx, source_root, destination):
    discovered = repository_ctx.execute(
        ["find", "-L", str(source_root), "-type", "f"],
        quiet = True,
    )
    if discovered.return_code != 0:
        fail("failed to enumerate %s: %s" % (source_root, discovered.stderr))
    prefix = str(source_root) + "/"
    for source in discovered.stdout.splitlines():
        if source.startswith(prefix):
            repository_ctx.symlink(
                source,
                destination + "/" + source[len(prefix):],
            )

def _k4b_cedarx_repository_impl(repository_ctx):
    include_directory = repository_ctx.os.environ.get(
        "K4B_CEDARX_INCLUDE_DIR",
        "",
    )
    library_directory = repository_ctx.os.environ.get(
        "K4B_CEDARX_LIB_DIR",
        "",
    )
    if not include_directory or not library_directory:
        # Keep repository/package discovery query-safe so graph validation can
        # decide whether the private K4B consumer is selected. The declared
        # source files remain absent, so analyzing or building that consumer
        # still fails closed instead of accepting an incomplete SDK.
        repository_ctx.file(
            "BUILD.bazel",
            repository_ctx.read(repository_ctx.attr.build_file),
        )
        error_header = (
            "#error K4B CedarX SDK is not configured; source .env/devenv " +
            "to set K4B_CEDARX_INCLUDE_DIR and K4B_CEDARX_LIB_DIR\n"
        )
        repository_ctx.file("include/vdecoder.h", error_header)
        repository_ctx.file("include/memoryAdapter.h", error_header)
        return
    include_root = repository_ctx.path(include_directory)
    library_root = repository_ctx.path(library_directory)
    required_libraries = [
        "libMemAdapter.so",
        "libVE.so",
        "libaftertreatment.so",
        "libawh264.so",
        "libcdc_base.so",
        "libcdx_ion.so",
        "libfbm.so",
        "libsbm.so",
        "libscaledown.so",
        "libvdecoder.so",
        "libvideoengine.so",
    ]
    for root, marker in [
        (include_root, "vdecoder.h"),
        (include_root, "memoryAdapter.h"),
    ] + [(library_root, library) for library in required_libraries]:
        if not root.get_child(marker).exists:
            fail("K4B CedarX SDK path %s does not contain %s" % (root, marker))
    _symlink_header_tree(repository_ctx, include_root, "include")
    for library in required_libraries:
        repository_ctx.symlink(
            library_root.get_child(library),
            "lib/" + library,
        )
    repository_ctx.file(
        "BUILD.bazel",
        repository_ctx.read(repository_ctx.attr.build_file),
    )

_k4b_cedarx_repository = repository_rule(
    implementation = _k4b_cedarx_repository_impl,
    attrs = {
        "build_file": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
    },
    environ = [
        "K4B_CEDARX_INCLUDE_DIR",
        "K4B_CEDARX_LIB_DIR",
    ],
    local = True,
)

_repository = tag_class(
    attrs = {
        "build_file": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "name": attr.string(mandatory = True),
        "ffmpeg_configure_adapter": attr.bool(default = False),
        "nested_archive_sha256": attr.string_dict(),
        "nested_archive_strip_prefix": attr.string_dict(),
        "nested_archive_urls": attr.string_dict(),
        "overlay_files": attr.label_keyed_string_dict(allow_files = True),
        "path": attr.string(),
        "patches": attr.label_list(allow_files = True),
        "sha256": attr.string(),
        "strip_prefix": attr.string(),
        "urls": attr.string_list(),
    },
)

def _vendor_repositories_impl(module_ctx):
    _fdk_aac_repository(
        name = "h2_fdk_aac",
        build_file = "//tools/bazel/vendor:fdk_aac.BUILD.bazel",
        overlay_files = {
            "//third_party/fdk_aac_patch:libFDK/include/common_fix.h": "libFDK/include/common_fix.h",
            "//third_party/fdk_aac_patch:libFDK/include/fixmul.h": "libFDK/include/fixmul.h",
            "//third_party/fdk_aac_patch:libFDK/include/fixpoint_math.h": "libFDK/include/fixpoint_math.h",
        },
        patches = ["//third_party/fdk_aac_patch:fdk_aac_embedded_no_stdio.patch"],
    )
    _k4b_cedarx_repository(
        name = "h2_k4b_cedarx_sdk",
        build_file = "//tools/bazel/vendor:kickpi_k4b_cedarx.BUILD.bazel",
    )
    for module in module_ctx.modules:
        for repository in module.tags.repository:
            if repository.urls:
                if repository.path:
                    fail("vendor repository %s cannot set both urls and path" % repository.name)
                _archive_repository(
                    name = repository.name,
                    build_file = repository.build_file,
                    ffmpeg_configure_adapter = repository.ffmpeg_configure_adapter,
                    nested_archive_sha256 = repository.nested_archive_sha256,
                    nested_archive_strip_prefix = repository.nested_archive_strip_prefix,
                    nested_archive_urls = repository.nested_archive_urls,
                    overlay_files = repository.overlay_files,
                    patches = repository.patches,
                    sha256 = repository.sha256,
                    strip_prefix = repository.strip_prefix,
                    urls = repository.urls,
                )
            else:
                if not repository.path:
                    fail("vendor repository %s requires urls or path" % repository.name)
                _local_vendor_repository(
                    name = repository.name,
                    build_file = repository.build_file,
                    ffmpeg_configure_adapter = repository.ffmpeg_configure_adapter,
                    overlay_files = repository.overlay_files,
                    patches = repository.patches,
                    path = repository.path,
                )

vendor_repositories = module_extension(
    implementation = _vendor_repositories_impl,
    tag_classes = {"repository": _repository},
)
