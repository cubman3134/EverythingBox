#!/usr/bin/env bash
# Auto-bump the PATCH version on every commit (0.4.0 -> 0.4.1 -> ...), keeping the two
# version sites in sync: native/CMakeLists.txt project(VERSION) and kAppVersion in
# native/src/main.cpp. Installed as .git/hooks/pre-commit (shared by all worktrees).
#
# NOTE: this hook is MANUALLY installed (copy/symlink into .git/hooks/pre-commit) — it is NOT
# wired up automatically on a fresh clone, so a new checkout won't bump the version until you
# install it. It also `git add`s the two version files wholesale, so it OVER-STAGES any other
# unrelated in-flight edits already sitting in those two files (CMakeLists.txt / main.cpp) into
# the commit. Keep those two files clean of unstaged work when committing, or stage deliberately.
#
# Skip rules:
#  - during a MERGE (or a conflicted revert), do nothing — a merge inherits the merged
#    branch's version and must not invent its own (see #181, below).
#  - if this commit ALREADY changes the version line (a manual bump like a release, or
#    an --amend where the hook already ran), do nothing — prevents double-bumps.
#  - EB_NO_VERSION_BUMP=1 in the environment skips (escape hatch).
set -e
[ "${EB_NO_VERSION_BUMP:-0}" = "1" ] && exit 0

# Do NOT bump during a merge. A merge commit introduces no new source that needs a fresh build
# identity — the branch being merged already carries its own bump, and the merge inherits it.
# Bumping here is wrong twice over (#181):
#   1. The bump is staged during merge finalization, but the merge commit need not capture it,
#      stranding a higher version in the working tree/index that exists in no commit — so the
#      next build reports a version main never had.
#   2. The leftover staged change makes the *next* `git merge` abort with "Your local changes
#      would be overwritten by merge", which reads as a conflict in a tree that has none.
# MERGE_HEAD (and REVERT_HEAD for a conflicted revert) exists for the whole duration of the
# operation, including `--no-commit` and manual conflict resolution. `--git-path` resolves the
# per-worktree git dir correctly in linked worktrees.
if [ -f "$(git rev-parse --git-path MERGE_HEAD)" ] || [ -f "$(git rev-parse --git-path REVERT_HEAD)" ]; then
    exit 0
fi

root=$(git rev-parse --show-toplevel)
cml="$root/native/CMakeLists.txt"
mcpp="$root/native/src/main.cpp"
[ -f "$cml" ] && [ -f "$mcpp" ] || exit 0

# Already-bumped-in-this-commit guard: staged CMakeLists version differs from HEAD's.
staged_ver=$(git show :native/CMakeLists.txt 2>/dev/null | grep -oE 'EverythingBoxNative VERSION [0-9]+\.[0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || true)
head_ver=$(git show HEAD:native/CMakeLists.txt 2>/dev/null | grep -oE 'EverythingBoxNative VERSION [0-9]+\.[0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || true)
if [ -n "$staged_ver" ] && [ -n "$head_ver" ] && [ "$staged_ver" != "$head_ver" ]; then
    exit 0
fi

cur=$(grep -oE 'EverythingBoxNative VERSION [0-9]+\.[0-9]+\.[0-9]+' "$cml" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
[ -n "$cur" ] || exit 0
maj=${cur%%.*}; rest=${cur#*.}; min=${rest%%.*}; pat=${rest#*.}
new="$maj.$min.$((pat + 1))"

sed -i "s/EverythingBoxNative VERSION $cur/EverythingBoxNative VERSION $new/" "$cml"
sed -i "s/kAppVersion = \"$cur\"/kAppVersion = \"$new\"/" "$mcpp"
git add "$cml" "$mcpp"
exit 0
