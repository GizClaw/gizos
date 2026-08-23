#!/bin/sh
set -eu

git submodule sync --recursive

# In a linked worktree, submodule clones normally re-download and re-store the
# full object database under .git/worktrees/<name>/modules, costing ~2.5 GB per
# worktree. The main checkout's stores under .git/modules already have (almost)
# every object, so clone each submodule with --reference to them: git records
# an "alternates" link and only fetches objects the shared store is missing.
#
# Caveat of alternates: the shared store must not prune objects the borrowing
# worktrees still need. Objects reachable from upstream refs are never pruned
# by a normal gc, so avoid manually deleting stores under .git/modules while
# linked worktrees exist.
common_dir=$(git rev-parse --path-format=absolute --git-common-dir)

git config -f .gitmodules --get-regexp '^submodule\..*\.path$' |
while read -r key path; do
  name=${key#submodule.}
  name=${name%.path}
  ref="$common_dir/modules/$name"
  # git clone refuses a shallow repository as --reference; fall back then.
  if [ -d "$ref/objects" ] && [ ! -f "$ref/shallow" ]; then
    git submodule update --init --reference "$ref" -- "$path"
  else
    git submodule update --init -- "$path"
  fi
done

# Second pass for nested submodules (and anything the loop missed). The
# per-submodule --reference above must not recurse: nested submodules would
# inherit the wrong reference path.
git submodule update --init --recursive

# Final pass, linked worktrees only: dedupe every submodule store (nested ones
# included, which the clone-time --reference cannot cover) against the shared
# stores via alternates + repack -l, and hardlink-share git-lfs objects.
git_dir=$(git rev-parse --path-format=absolute --git-dir)
[ "$git_dir" = "$common_dir" ] && exit 0
find "$git_dir/modules" -type d -name objects 2>/dev/null | while read -r objdir; do
  d=${objdir%/objects}
  [ -f "$d/HEAD" ] || continue
  rel=${d#"$git_dir"/modules/}
  main="$common_dir/modules/$rel"
  [ -d "$main/objects" ] || continue
  [ -f "$main/shallow" ] && continue
  mkdir -p "$d/objects/info"
  grep -qxF "$main/objects" "$d/objects/info/alternates" 2>/dev/null ||
    echo "$main/objects" >> "$d/objects/info/alternates"
  git --git-dir="$d" repack -a -d -l -q || continue
  git --git-dir="$d" prune-packed -q || true
  if [ -d "$d/lfs/objects" ]; then
    find "$d/lfs/objects" -type f | while read -r f; do
      o="$main/lfs/objects/${f#"$d"/lfs/objects/}"
      if [ -f "$o" ]; then
        [ "$f" -ef "$o" ] || ln -f "$o" "$f"
      else
        mkdir -p "${o%/*}" && ln "$f" "$o" 2>/dev/null || true
      fi
    done
  fi
done
