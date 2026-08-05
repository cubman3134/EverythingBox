#!/usr/bin/env bash
# Install the repo's git-side version tooling. This is PER-CLONE: neither the pre-commit
# hook nor the merge driver is active on a fresh clone until you run this. Re-running is
# safe (idempotent). Works from any linked worktree — the hook lands in the shared hooks
# dir and the merge-driver config lands in the shared repo config, so one run covers every
# worktree of this clone.
#
# It installs two things, and they work together (issue #181):
#   1. pre-commit-version-bump.sh -> .git/hooks/pre-commit
#        Bumps the patch version on every ordinary commit. It declines on a merge
#        (MERGE_HEAD/REVERT_HEAD guard) so a merge never invents its own version.
#   2. merge driver `ebversion` on native/CMakeLists.txt and native/src/main.cpp
#        Auto-resolves the version lines to the HIGHER of the two sides. Because those
#        lines no longer conflict, a version-only merge finishes automatically with no
#        manual `git commit` — and it was that manual commit that used to strand a staged
#        bump the merge commit never captured.
set -eu

root=$(git rev-parse --show-toplevel)
hooks_dir=$(git rev-parse --git-path hooks)
mkdir -p "$hooks_dir"

# 1. pre-commit hook.
cp "$root/native/tools/pre-commit-version-bump.sh" "$hooks_dir/pre-commit"
chmod +x "$hooks_dir/pre-commit"
echo "installed pre-commit hook -> $hooks_dir/pre-commit"

# 2. merge driver. `bash <script>` so it runs regardless of the exec bit / CRLF checkout;
#    the relative path resolves against the worktree top-level, where git runs the driver.
git config merge.ebversion.name "EverythingBox version-line merge (keep the higher version)"
git config merge.ebversion.driver "bash native/tools/merge-version.sh %O %A %B %P"
echo "configured merge driver 'ebversion' (native/CMakeLists.txt, native/src/main.cpp)"

echo
echo "Done. Both are per-clone; a fresh clone must run this again."
