#!/usr/bin/env bash
# Run the `app link` gate (issue #182) ON ITS OWN, out of run-headless-probes.sh.
#
# WHY THIS EXISTS. CONTRIBUTING.md's rule is that an assertion is proven by breaking the behaviour it guards and
# watching it go red. The app-link gate's break is "add a symbol the app uses to a probe target but not to
# qt_add_executable(everythingbox ...)" — #128's actual failure — and the only way to reach the gate otherwise
# is to run the whole ~120-probe suite, which is nearly three minutes per attempt and rebuilds nothing relevant.
#
# So: extract the gate's own lines and run them. It reads the SAME text the suite runs, never a copy, because a
# copy would drift and then this would be proving a gate that is not the one in CI. Same shape as its two
# neighbours, native/tools/themeprops-gate-check.sh and native/tools/leafroute-gate-check.sh.
#
# Usage:  BUILD_DIR=build ./native/tools/applink-gate-check.sh          (single-config generator)
#         BUILD_DIR=build/Release ./native/tools/applink-gate-check.sh  (multi-config; picks that config up)
# Exit 0 = the app builds, 1 = the gate went red, 2 = the extraction is meaningless (see the floor below).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNER="$HERE/run-headless-probes.sh"

[ -f "$RUNNER" ] || { echo "applink-gate-check: $RUNNER not found"; exit 2; }

SECTION="$(mktemp)"
trap 'rm -f "$SECTION"' EXIT
# From the gate's banner to the blank `echo` that closes it. A file operand is always supplied so awk cannot
# fall back to this script's own stdin.
awk '/^echo "=== app link ==="/ { p = 1 } p { print } p && /^echo$/ && NR > 1 && !/===/ { exit }' \
    "$RUNNER" </dev/null > "$SECTION"

# The extraction is itself a thing that can silently do nothing, and an empty section would run clean and report
# the app as fine — which is the exact verdict issue #182 is about. Floor is well under the gate's real size; if
# the banner is renamed this stops rather than passing.
lines="$(wc -l < "$SECTION" | tr -d '[:space:]')"
if [ "$lines" -lt 60 ]; then
  echo "applink-gate-check: extracted $lines line(s) from $RUNNER — the gate's banner has moved or been"
  echo "renamed, so NOTHING was checked. Treat any verdict from this run as meaningless."
  exit 2
fi

# The two names the suite has already bound by the time the gate runs, and the one variable the gate reads
# optionally ($EXE_DIR, for picking the multi-config configuration — the gate defaults it to $BUILD_DIR).
BUILD_DIR="${BUILD_DIR:-build}"
NATIVE_DIR="$(cd "$HERE/.." && pwd)"
export BUILD_DIR NATIVE_DIR

fail=0
# shellcheck disable=SC1090
. "$SECTION"
exit "$fail"
