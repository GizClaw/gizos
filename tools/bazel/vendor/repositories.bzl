"""Bzlmod repositories for pinned gitlinks and Linux system libraries."""

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

def _validate_gitlink_revision(repository_ctx, workspace_root, source_root):
    indexed = repository_ctx.execute(
        [
            "git",
            "-C",
            str(workspace_root),
            "ls-files",
            "--stage",
            "--",
            repository_ctx.attr.path,
        ],
        quiet = True,
    )
    if indexed.return_code != 0:
        fail(
            "failed to inspect gitlink %s: %s" %
            (repository_ctx.attr.path, indexed.stderr),
        )
    entries = indexed.stdout.strip().splitlines()
    if len(entries) != 1 or not entries[0].startswith("160000 "):
        fail(
            (
                "%s is not exactly one Git index gitlink entry; restore the " +
                "pinned submodule before building"
            ) % repository_ctx.attr.path,
        )
    expected = entries[0].split(" ", 2)[1]
    checked_out = repository_ctx.execute(
        ["git", "-C", str(source_root), "rev-parse", "HEAD"],
        quiet = True,
    )
    actual = checked_out.stdout.strip()
    if checked_out.return_code != 0 or actual != expected:
        fail(
            (
                "gitlink %s is at %s; expected %s; run git submodule update " +
                "--init --recursive"
            ) % (
                repository_ctx.attr.path,
                actual or "an unknown revision",
                expected,
            ),
        )

def _gitlink_repository_impl(repository_ctx):
    workspace_root = repository_ctx.path(repository_ctx.attr.workspace_file).dirname
    source_root = workspace_root.get_child(repository_ctx.attr.path)
    if not source_root.exists:
        fail(
            "gitlink %s is missing; run git submodule update --init --recursive" %
            repository_ctx.attr.path,
        )
    repository_ctx.watch_tree(source_root)
    if repository_ctx.attr.source_is_gitlink:
        _validate_gitlink_revision(repository_ctx, workspace_root, source_root)
    discovered = _source_files(source_root)
    if not discovered:
        fail(
            "gitlink %s is empty; run git submodule update --init --recursive" %
            repository_ctx.attr.path,
        )
    patched_paths = {}
    for patch in repository_ctx.attr.patches:
        for line in repository_ctx.read(patch).splitlines():
            if line.startswith("+++ "):
                path = line[4:].split("\t", 1)[0]
                if path != "/dev/null":
                    patched_paths[path] = True

    for source in discovered:
        destination = "src/" + source.relative
        if destination in patched_paths:
            repository_ctx.file(destination, repository_ctx.read(source.path))
        else:
            repository_ctx.symlink(source.path, destination)
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

_gitlink_repository = repository_rule(
    implementation = _gitlink_repository_impl,
    attrs = {
        "build_file": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "ffmpeg_configure_adapter": attr.bool(default = False),
        "overlay_files": attr.label_keyed_string_dict(allow_files = True),
        "path": attr.string(mandatory = True),
        "patches": attr.label_list(allow_files = True),
        "source_is_gitlink": attr.bool(default = True),
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

_fdk_aac_repository = repository_rule(
    implementation = _fdk_aac_repository_impl,
    attrs = {
        "build_file": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "overlay_files": attr.label_keyed_string_dict(allow_files = True),
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
        "overlay_files": attr.label_keyed_string_dict(allow_files = True),
        "path": attr.string(mandatory = True),
        "patches": attr.label_list(allow_files = True),
        "source_is_gitlink": attr.bool(default = True),
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
    )
    _k4b_cedarx_repository(
        name = "h2_k4b_cedarx_sdk",
        build_file = "//tools/bazel/vendor:kickpi_k4b_cedarx.BUILD.bazel",
    )
    for module in module_ctx.modules:
        for repository in module.tags.repository:
            _gitlink_repository(
                name = repository.name,
                build_file = repository.build_file,
                ffmpeg_configure_adapter = repository.ffmpeg_configure_adapter,
                overlay_files = repository.overlay_files,
                path = repository.path,
                patches = repository.patches,
                source_is_gitlink = repository.source_is_gitlink,
            )

vendor_repositories = module_extension(
    implementation = _vendor_repositories_impl,
    tag_classes = {"repository": _repository},
)
