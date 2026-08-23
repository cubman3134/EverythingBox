#!/usr/bin/env bash
# Run the `themed local-leaf routing parity` gate ON ITS OWN, out of run-headless-probes.sh.
#
# WHY THIS EXISTS. CONTRIBUTING.md's rule is that an assertion is proven by breaking the behaviour it guards
# and watching it go red, and that the driver for that loop is tools/mutate.py. A source gate is an assertion
# like any other — it is the one that would have caught the shipped "Nothing to play" bug — so it has to be
# mutation-proven too. mutate.py needs a `test` command per mutant, and the only way to reach this gate was to
# run the whole hundred-probe suite, which is minutes per mutant and rebuilds nothing relevant.
#
# So: extract the gate's own lines and run them. It reads the SAME text the suite runs, never a copy, because
# a copy would drift and then this would be proving a gate that is not the one in CI.
#
# Used by native/tools/leafroute-mutants.json. Also handy by hand while editing the gate.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNER="$HERE/run-headless-probes.sh"

[ -f "$RUNNER" ] || { echo "leafroute-gate-check: $RUNNER not found"; exit 2; }

SECTION="$(mktemp)"
trap 'rm -f "$SECTION"' EXIT
# From the gate's banner to the blank `echo` that closes it. A file operand is always supplied so awk cannot
# fall back to this script's own stdin.
awk '/^echo "=== themed local-leaf routing parity ==="/ { p = 1 } p { print } p && /^echo$/ && NR > 1 && !/===/ { exit }' \
    "$RUNNER" </dev/null > "$SECTION"

# The extraction is itself a thing that can silently do nothing, and an empty section would run clean and
# report every mutant as SURVIVED — which is the verdict that gets a working assertion deleted. Floor is well
# under the gate's real size; if the banner is renamed this stops rather than passing.
lines="$(wc -l < "$SECTION" | tr -d '[:space:]')"
if [ "$lines" -lt 40 ]; then
  echo "leafroute-gate-check: extracted $lines line(s) from $RUNNER — the gate's banner has moved or been"
  echo "renamed, so NOTHING was checked. Treat any verdict from this run as meaningless."
  exit 2
fi

fail=0
# shellcheck disable=SC1090
. "$SECTION"
exit "$fail"
