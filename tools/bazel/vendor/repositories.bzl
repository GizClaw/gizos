"""Bzlmod repositories for pinned public gitlinks."""

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

_gitlink_repository = repository_rule(
    implementation = _gitlink_repository_impl,
    attrs = {
        "build_file": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
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

_repository = tag_class(
    attrs = {
        "build_file": attr.label(
            allow_single_file = True,
            mandatory = True,
        ),
        "name": attr.string(mandatory = True),
        "overlay_files": attr.label_keyed_string_dict(allow_files = True),
        "path": attr.string(mandatory = True),
        "patches": attr.label_list(allow_files = True),
        "source_is_gitlink": attr.bool(default = True),
    },
)

def _vendor_repositories_impl(module_ctx):
    for module in module_ctx.modules:
        for repository in module.tags.repository:
            _gitlink_repository(
                name = repository.name,
                build_file = repository.build_file,
                overlay_files = repository.overlay_files,
                path = repository.path,
                patches = repository.patches,
                source_is_gitlink = repository.source_is_gitlink,
            )

vendor_repositories = module_extension(
    implementation = _vendor_repositories_impl,
    tag_classes = {"repository": _repository},
)
