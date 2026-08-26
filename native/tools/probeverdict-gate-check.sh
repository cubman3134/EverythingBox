#!/usr/bin/env bash
# Prove the SUITE'S OWN VERDICT (issue #180) — on its own, out of run-headless-probes.sh.
#
# WHY THIS EXISTS. The verdict is the one line every merge in this repo is reported through, and three times it
# said SOME HEADLESS PROBES FAILED while naming no probe. The fix makes the verdict a function of the
# transcript — the PASS:/FAIL: lines the run actually printed, plus the status it exited with — so the summary
# and the evidence cannot drift. That is a claim about a piece of shell, and CONTRIBUTING.md's rule is that a
# claim is proven by breaking what it guards and watching it go red.
#
# Reaching it through the suite means a three-minute run per attempt, and worse, the interesting inputs (a
# probe that dies without printing anything; a run whose status disagrees with its lines) are exactly the ones
# you cannot produce on demand — the flake behind #180 is about 1 run in 8. So: extract the verdict's own lines
# and feed them hand-written transcripts. It reads the SAME text the suite runs, never a copy, because a copy
# would drift and then this would be proving a verdict that is not the one CI reports through. Same shape as
# native/tools/applink-gate-check.sh, themeprops-gate-check.sh and leafroute-gate-check.sh.
#
# Usage:  bash native/tools/probeverdict-gate-check.sh
# Exit 0 = the verdict behaves, 1 = it went red, 2 = the extraction is meaningless (see the floor below).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNER="$HERE/run-headless-probes.sh"

[ -f "$RUNNER" ] || { echo "probeverdict-gate-check: $RUNNER not found"; exit 2; }

SECTION="$(mktemp)"
WORK="$(mktemp -d)"
trap 'rm -rf "$SECTION" "$WORK"' EXIT

# Banner to banner. A file operand is always supplied so awk cannot fall back to this script's own stdin.
awk '/VERDICT BLOCK BEGIN/ { p = 1 } p { print } p && /VERDICT BLOCK END/ { exit }' "$RUNNER" </dev/null > "$SECTION"

# The extraction is itself a thing that can silently do nothing, and an empty section would define no
# suite_verdict at all — every scenario below would then fail for the wrong reason, or (worse, if this file
# ever grows a "no news is good news" path) pass. Floor is well under the block's real size.
lines="$(wc -l < "$SECTION" | tr -d '[:space:]')"
if [ "$lines" -lt 80 ]; then
  echo "probeverdict-gate-check: extracted $lines line(s) from $RUNNER — the VERDICT BLOCK banners have moved"
  echo "or been renamed, so NOTHING was checked. Treat any verdict from this run as meaningless."
  exit 2
fi

BUILD_DIR="${BUILD_DIR:-build}"
export BUILD_DIR
# shellcheck disable=SC1090
. "$SECTION"

command -v suite_verdict >/dev/null 2>&1 || {
  echo "probeverdict-gate-check: the extracted section defines no suite_verdict — nothing was checked."; exit 2; }

T="$WORK/transcript.log"
OUT="$WORK/stdout"
ERR="$WORK/stderr"
EB_PROBE_VERDICT="$WORK/verdict"
export EB_PROBE_VERDICT

gate_fail=0
CASE=""
case_fail=0
# `ok` reports only when the case had no RED in it — a line that says "ok" under a failure it just printed is
# the small version of the very confusion this file exists to fix.
bad() { echo "  RED  [$CASE] $*"; gate_fail=1; case_fail=1; }
ok()  { [ "$case_fail" -eq 0 ] && echo "  ok   [$CASE] $*"; case_fail=0; return 0; }

# A transcript that looks like a real run: <n> passing checks, then whatever extra lines the case needs.
mk() { # <n passes> [extra line ...]
  local n="$1" i=1 l; shift
  : > "$T"
  while [ "$i" -le "$n" ]; do printf '=== check %s ===\nPASS: check %s\n' "$i" "$i" >> "$T"; i=$((i + 1)); done
  for l in "$@"; do printf '%s\n' "$l" >> "$T"; done
}

verdict() { # <exit status of the run that produced the transcript>
  rm -f "$EB_PROBE_VERDICT"
  suite_verdict "$T" "$1" > "$OUT" 2> "$ERR"
  VRC=$?
}

want_rc()    { [ "$VRC" -eq "$1" ] || bad "expected exit $1, got $VRC"; }
want_out()   { grep -qF -- "$1" "$OUT" || bad "stdout never says: $1"; }
want_noout() { grep -qF -- "$1" "$OUT" && bad "stdout should not say: $1"; return 0; }
want_last()  { tail -1 "$OUT" | grep -qF -- "$1" || bad "last line is '$(tail -1 "$OUT")' — expected it to carry: $1"; }
want_err()   { grep -qF -- "$1" "$ERR" || bad "stderr never says: $1"; }
want_file()  { grep -qF -- "$1" "$EB_PROBE_VERDICT" 2>/dev/null || bad "the verdict file never says: $1"; }

echo "probeverdict-gate-check: $lines line(s) extracted from $(basename "$RUNNER")"

# ---- 1. a clean run is a pass, and says so in the words every caller greps for -----------------------------
CASE="clean run"
mk 120
verdict 0
want_rc 0
want_last "ALL HEADLESS PROBES PASSED"
want_file "VERDICT=PASS"
want_file "PASSED=120"
[ -s "$ERR" ] && bad "a passing run wrote to stderr: $(head -1 "$ERR")"
ok "120 PASS lines + exit 0 -> ALL HEADLESS PROBES PASSED, verdict file says PASS"

# ---- 2. a named failure is named, on the LAST line ---------------------------------------------------------
# The `tail -3` sighting on #180: a caller who sees only the end of the output must still learn which probe.
CASE="named failure"
mk 120 "=== probe_books ===" "FAIL: probe_books (rc=1, it printed 4 line(s), expected 'BOOKS-OK')"
verdict 1
want_rc 1
want_out "FAIL: probe_books"
want_last "SOME HEADLESS PROBES FAILED: probe_books"
want_err "SOME HEADLESS PROBES FAILED: probe_books"
want_file "VERDICT=FAIL"
ok "the failing probe is named in the verdict, on the last line, and on stderr"

# ---- 3. a probe that DIES without printing anything is still named -----------------------------------------
# This is the case the old counter could not express: rc=139 mid-suite, no FAIL: line anywhere, and a summary
# that could only say "something". The attributed sighting on #180 is exactly this shape.
CASE="silent death"
mk 40 "=== probe_uitest ===" "uitest: opening its own channel"
verdict 139
want_rc 1
want_out "probe_uitest"
want_out "SIGSEGV"
want_out "WITHOUT printing a single FAIL: line"
want_last "SOME HEADLESS PROBES FAILED: probe_uitest (vanished: rc=139)"
want_file "VERDICT=FAIL"
ok "a run that vanished at rc=139 names the section it vanished in and how it died"

# ---- 4. the two records disagreeing is itself a failure ----------------------------------------------------
# FAIL: lines printed, status 0. Before #180 the status WAS the verdict, so this run reported a pass.
CASE="lines say fail, status says pass"
mk 120 "FAIL: exe-folder contamination — something changed the folder the app's data lives in during the run."
verdict 0
want_rc 1
want_out "exe-folder contamination"
want_out "the run's own exit status was 0"
want_last "SOME HEADLESS PROBES FAILED: exe-folder contamination"
ok "a zero status cannot outvote a printed FAIL: line"

# ---- 5. ... and in the other direction ---------------------------------------------------------------------
# No FAIL: lines, plenty of passes, but the run exited non-zero: something failed where it could not be seen.
CASE="status says fail, lines say pass"
mk 120
verdict 3
want_rc 1
want_out "WITHOUT printing a single FAIL: line"
want_last "SOME HEADLESS PROBES FAILED"
ok "a non-zero status cannot be outvoted by silence either"

# ---- 6. a run too short to have asserted anything is not a pass --------------------------------------------
# The corpus floor its neighbouring gates all carry: a suite that died after four checks, or a parse that
# stopped matching, must not read as "nothing failed".
CASE="thin transcript"
mk 4
verdict 0
want_rc 1
want_out "under the floor of"
want_noout "ALL HEADLESS PROBES PASSED"
ok "4 PASS lines and a zero status is a FAIL, not a quiet pass"

# ---- 7. several failures, all named, still one line ---------------------------------------------------------
CASE="several failures"
mk 120 "FAIL: probe_uitest (rc=139 (killed by SIGSEGV - it crashed, so it never reached its own reporting), it printed nothing at all, expected 'UITEST-OK')" \
       "FAIL: APP LINK — the everythingbox APP TARGET DID NOT BUILD (cmake --build exited 1)."
verdict 1
want_rc 1
want_last "probe_uitest"
want_last "APP LINK"
want_out "killed by SIGSEGV"
ok "two failures, both named on the last line, with how the first one died"

# ---- 8. no transcript at all is a failure, not a pass -------------------------------------------------------
CASE="no transcript"
rm -f "$T"
verdict 0
want_rc 1
want_last "SOME HEADLESS PROBES FAILED"
ok "a missing transcript cannot be read as a clean run"

echo
if [ "$gate_fail" -eq 0 ]; then
  echo "probeverdict-gate-check: PASS — the verdict is derived from the transcript in all 8 cases"
else
  echo "probeverdict-gate-check: FAIL — the verdict does not follow the per-probe lines (see RED above)"
fi
exit "$gate_fail"
