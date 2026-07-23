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
#  - if this commit ALREADY changes the version line (a manual bump like a release, or
#    an --amend where the hook already ran), do nothing — prevents double-bumps.
#  - MMV_NO_VERSION_BUMP=1 in the environment skips (escape hatch).
set -e
[ "${MMV_NO_VERSION_BUMP:-0}" = "1" ] && exit 0
root=$(git rev-parse --show-toplevel)
cml="$root/native/CMakeLists.txt"
mcpp="$root/native/src/main.cpp"
[ -f "$cml" ] && [ -f "$mcpp" ] || exit 0

# Already-bumped-in-this-commit guard: staged CMakeLists version differs from HEAD's.
staged_ver=$(git show :native/CMakeLists.txt 2>/dev/null | grep -oE 'MyMediaVaultNative VERSION [0-9]+\.[0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || true)
head_ver=$(git show HEAD:native/CMakeLists.txt 2>/dev/null | grep -oE 'MyMediaVaultNative VERSION [0-9]+\.[0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || true)
if [ -n "$staged_ver" ] && [ -n "$head_ver" ] && [ "$staged_ver" != "$head_ver" ]; then
    exit 0
fi

cur=$(grep -oE 'MyMediaVaultNative VERSION [0-9]+\.[0-9]+\.[0-9]+' "$cml" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
[ -n "$cur" ] || exit 0
maj=${cur%%.*}; rest=${cur#*.}; min=${rest%%.*}; pat=${rest#*.}
new="$maj.$min.$((pat + 1))"

sed -i "s/MyMediaVaultNative VERSION $cur/MyMediaVaultNative VERSION $new/" "$cml"
sed -i "s/kAppVersion = \"$cur\"/kAppVersion = \"$new\"/" "$mcpp"
git add "$cml" "$mcpp"
exit 0
