#!/usr/bin/env bash
# Custom git merge driver for the two version-bearing files (issue #181):
#   native/CMakeLists.txt   -> project(EverythingBoxNative VERSION x.y.z ...)
#   native/src/main.cpp     -> static constexpr ... kAppVersion = "x.y.z";
#
# Why this exists. The pre-commit hook bumps the patch version per-commit-per-branch,
# so ANY two feature branches diverge on those version lines and every merge of them
# conflicts on exactly those lines. A hand-resolved conflict forces a manual
# `git commit`, and that manual commit is the path that stranded a staged bump in the
# index (the deployed binary reported a version no commit contained, #181). Remove the
# conflict and the whole failure mode goes away.
#
# What it does. Given git's %O (base) %A (ours) %B (theirs) %P (path), it rewrites the
# version token in all three to the HIGHER of ours/theirs, then runs an ordinary 3-way
# merge. Because the version line is then identical across base/ours/theirs, it merges
# with no conflict and yields the higher version. Any OTHER real conflict in the file is
# left untouched (non-zero exit), so genuine code conflicts still surface normally.
# "Higher" is deliberate: the bump rule always keeps the higher version, and neither side
# is reliably ahead of the other.
#
# EOL-agnostic on purpose: it only ever substitutes a run of digits/dots, so CRLF vs LF
# in the temp files git hands us is irrelevant and the surrounding bytes are preserved.
#
# Installed per-clone by native/tools/install-git-hooks.sh (git config merge.ebversion.*).
set -u

O="$1"      # %O ancestor/base
A="$2"      # %A ours   (result MUST be written here)
B="$3"      # %B theirs
P="${4:-}"  # %P pathname in the working tree (informational only)

# Pull the x.y.z out of whichever version line a file carries.
extract_ver() {
    grep -oE 'EverythingBoxNative VERSION [0-9]+\.[0-9]+\.[0-9]+|kAppVersion = "[0-9]+\.[0-9]+\.[0-9]+"' "$1" 2>/dev/null \
        | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1
}

# Echo the higher of two dotted numeric versions.
higher_ver() {
    if [ "$1" = "$2" ]; then printf '%s\n' "$1"; return; fi
    printf '%s\n%s\n' "$1" "$2" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1
}

# Rewrite whichever version line exists in file $1 to version $2 (digits only; EOL kept).
set_ver() {
    sed -i -E \
        -e "s/(EverythingBoxNative VERSION )[0-9]+\.[0-9]+\.[0-9]+/\1$2/" \
        -e "s/(kAppVersion = \")[0-9]+\.[0-9]+\.[0-9]+(\")/\1$2\2/" \
        "$1"
}

ours=$(extract_ver "$A")
theirs=$(extract_ver "$B")

# Only special-case the merge when BOTH sides carry a recognizable version token.
# Otherwise fall through to a plain 3-way merge so nothing is silently swallowed.
if [ -n "$ours" ] && [ -n "$theirs" ]; then
    win=$(higher_ver "$ours" "$theirs")
    set_ver "$O" "$win"
    set_ver "$A" "$win"
    set_ver "$B" "$win"
fi

# Ordinary 3-way merge; result overwrites $A. Non-zero exit => a real conflict remains.
git merge-file -L ours -L base -L theirs "$A" "$O" "$B"
