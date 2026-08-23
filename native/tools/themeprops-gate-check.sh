#!/usr/bin/env bash
# Run the `themed host-property declarations` gate ON ITS OWN, out of run-headless-probes.sh.
#
# WHY THIS EXISTS. CONTRIBUTING.md's rule is that an assertion is proven by breaking the behaviour it guards
# and watching it go red. A source gate is an assertion like any other — this one is the assertion that would
# have caught `actionRomhack` and the .lrc lyrics trio, two features that shipped rendering nothing at all —
# so it has to be proven the same way. The only other route to it is the whole hundred-probe suite, which is
# minutes per attempt, needs a build, and rebuilds nothing this gate reads.
#
# So: extract the gate's own lines and run them. It reads the SAME text the suite runs, never a copy, because
# a copy would drift and then this would be proving a gate that is not the one in CI.
#
# Same shape and the same reasoning as leafroute-gate-check.sh next door.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNER="$HERE/run-headless-probes.sh"

[ -f "$RUNNER" ] || { echo "themeprops-gate-check: $RUNNER not found"; exit 2; }

SECTION="$(mktemp)"
trap 'rm -f "$SECTION"' EXIT
# From the gate's banner to the blank `echo` that closes it. A file operand is always supplied so awk cannot
# fall back to this script's own stdin.
awk '/^echo "=== themed host-property declarations ==="/ { p = 1 } p { print } p && /^echo$/ && NR > 1 && !/===/ { exit }' \
    "$RUNNER" </dev/null > "$SECTION"

# The extraction is itself a thing that can silently do nothing, and an empty section would run clean and
# report every mutant as SURVIVED — the verdict that gets a working assertion deleted. Floor is well under the
# gate's real size; if the banner is renamed this stops rather than passing.
lines="$(wc -l < "$SECTION" | tr -d '[:space:]')"
if [ "$lines" -lt 60 ]; then
  echo "themeprops-gate-check: extracted $lines line(s) from $RUNNER — the gate's banner has moved or been"
  echo "renamed, so NOTHING was checked. Treat any verdict from this run as meaningless."
  exit 2
fi

fail=0
# shellcheck disable=SC1090
. "$SECTION"
exit "$fail"
