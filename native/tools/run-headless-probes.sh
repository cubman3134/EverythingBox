#!/usr/bin/env bash
# Headless probe suite — the automated tests CI runs on every push/PR. These need no display, no GPU, and no
# ROMs: they spin up real subsystem code and assert a success sentinel.
#
#   * netplay relay        — two NetplaySession instances (two "emulators") pair through the relay, sync a save
#                            state, and exchange an input packet (probe_netplay -> NETPLAY-RELAY-OK).
#   * netplay both:direct  — the "Both" online orchestration with the relay dead, so ONLY a direct connection can
#                            pair them (probe_netplay_both direct -> NETPLAY-BOTH-OK).
#   * netplay both:relay   — same, but the direct endpoint is dead so it must fall back to the relay.
#   * netplay both:slowconnect — a host whose connect and whose answer each eat most of the joiner's give-up
#                            budget, but neither eats all of it: the joiner must still land on the direct path.
#   * netplay both:silent  — the direct endpoint accepts and then never handshakes (a stale port forward); the
#     / both:dropped         joiner must still reach the host over the relay instead of hanging or ending.
#   * core load (optional) — if $CORE_SO points at a real libretro core, dlopen it + run retro_init headlessly.
#
# Usage:  BUILD_DIR=build ./native/tools/run-headless-probes.sh
#         CORE_SO=/path/to/some_libretro.so BUILD_DIR=build ./native/tools/run-headless-probes.sh
#
# Qt platform plugin: the build now generates a qt.conf next to the probe exes (see
# native/CMakeLists.txt) whose [Paths] Plugins= points at Qt's plugins dir, so the probes
# find qwindows/qoffscreen on their own — QT_PLUGIN_PATH no longer has to be exported for a
# bare probe launch. Set QT_QPA_PLATFORM=offscreen in the env for the windowed probes to run
# headless (qt.conf supplies the plugin PATH, not the platform CHOICE).
set -uo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
# 0 = let the OS pick a free port for this run's relay, and read back what it picked. A hard-coded default meant
# two suites on one machine fought over one port: the loser's relay died silently, its probes talked to the
# winner's, and the netplay results stopped being about this run (issue #164). Override to pin it if you need to.
RELAY_PORT="${RELAY_PORT:-0}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELAY_PY="$HERE/netplay-relay.py"
PY="${PYTHON:-python3}"; command -v "$PY" >/dev/null 2>&1 || PY=python

# ---- The verdict, derived from the per-probe lines (issue #180) ---- VERDICT BLOCK BEGIN -------------------
# Three times this suite printed SOME HEADLESS PROBES FAILED while naming NO probe, and once a failing run was
# recorded as a pass because it was piped through `tail` (a pipeline reports TAIL's status, not the suite's).
# Both make a real failure look like noise, and noise is what people learn to re-run rather than investigate.
#
# The first is structural. The summary line used to be printed from a `fail` counter set at sixty-odd separate
# sites, while the evidence is the PASS:/FAIL: lines — two records of one fact, kept by hand, free to drift.
# A probe that DIES (rc=139 is the attribution on #180) never reaches its own reporting at all, so only the
# counter moved and the summary could not say what broke.
#
# So the verdict is no longer a second record of anything. The suite re-executes itself once with its whole
# output through `tee`, and the verdict is computed FROM THAT TRANSCRIPT: a run fails if a FAIL:/FATAL: line
# was printed, or if the run itself exited non-zero, or if those two disagree, or if too few PASS: lines came
# back for a pass to mean anything. `fail` decides exactly one thing now — the child's exit status — and that
# status is an INPUT here, cross-checked against the lines rather than trusted in place of them.
#
# native/tools/probeverdict-gate-check.sh extracts this block banner-to-banner and runs it standalone against
# hand-written transcripts, the same way applink/themeprops/leafroute-gate-check.sh prove their gates: it
# reads the same text CI runs, never a copy, so the proof cannot drift from the thing proven.

# A transcript with almost no PASS: lines in it cannot support "everything passed": the run died early, or the
# parse stopped matching. Deliberately far below the real count (157 at the time of writing) and far above
# zero — the same shape as the stale-binary gate's corpus guard above.
VERDICT_PASS_FLOOR="${VERDICT_PASS_FLOOR:-100}"

# What an exit status MEANS in the cases that are not "the probe said no". A process killed by a signal never
# got to print anything, and reporting that as a bare number is exactly how #180's sightings read as
# unattributable noise. 128+N is the shell's spelling of signal N; the raw NTSTATUS values turn up when a
# native probe faults and the runtime hands the code through unmasked.
# The second argument says whether the process had already PRINTED something, and 127 is why it exists: a
# 127 from a process that printed nothing is #205's signature (the exe or a DLL was missing, so it never
# ran), while a 127 from one that printed three lines is a process that died - the shell only has eight bits
# of exit status to report a Windows fault code in. Reading the first message onto the second case sends the
# next reader hunting for a missing DLL that is not missing.
exit_status_note() { # <rc> [1 if it had printed something] -> " (killed by SIGSEGV - ...)" or ""
  case "$1" in
    0)   printf '' ;;
    126) printf ' (126: found but not executable)' ;;
    127) if [ "${2:-0}" -eq 1 ]; then
           printf ' (127: it had already printed, so this is a process that DIED, not one that never ran)'
         else
           printf ' (127: the exe or a DLL it needs was not found, so it never ran - see #205)'
         fi ;;
    129) printf ' (killed by SIGHUP)' ;;
    130) printf ' (killed by SIGINT)' ;;
    131) printf ' (killed by SIGQUIT)' ;;
    132) printf ' (killed by SIGILL)' ;;
    133) printf ' (killed by SIGTRAP)' ;;
    134) printf ' (killed by SIGABRT - an assert, abort(), or a throw off the end)' ;;
    136) printf ' (killed by SIGFPE)' ;;
    137) printf ' (killed by SIGKILL)' ;;
    138) printf ' (killed by SIGBUS)' ;;
    139) printf ' (killed by SIGSEGV - it crashed, so it never reached its own reporting)' ;;
    141) printf ' (killed by SIGPIPE)' ;;
    143) printf ' (killed by SIGTERM)' ;;
    3221225477) printf ' (0xC0000005 ACCESS_VIOLATION - a Windows crash)' ;;
    3221225725) printf ' (0xC00000FD STACK_OVERFLOW)' ;;
    3221226505) printf ' (0xC0000409 fast-fail / stack cookie)' ;;
    *) if [ "$1" -gt 128 ] && [ "$1" -lt 192 ]; then printf ' (killed by signal %s)' "$(($1 - 128))"; fi ;;
  esac
}

# The verdict itself. Reads a transcript and the status of the run that produced it; prints the summary, writes
# it to a file a caller can check without trusting an exit status, and RETURNS the status the suite should
# exit with. Everything it says about the run comes out of the transcript.
suite_verdict() { # <transcript> <exit status of the run that produced it>
  local transcript="$1" child_rc="$2"
  local verdict_file="${EB_PROBE_VERDICT:-${BUILD_DIR:-build}/headless-probes.verdict}"

  if [ ! -f "$transcript" ]; then
    echo "The run left no transcript at '$transcript', so nothing can be derived from it: treat this run as"
    echo "having asserted NOTHING."
    echo "SOME HEADLESS PROBES FAILED: no transcript, no evidence, no verdict"
    return 1
  fi

  local fails n_fail n_pass last_section
  fails="$(grep -E '^(FAIL|FATAL): ' "$transcript" || true)"
  n_fail="$(printf '%s\n' "$fails" | grep -c '[^[:space:]]' || true)"
  n_pass="$(grep -c '^PASS: ' "$transcript" || true)"
  last_section="$(grep '^=== ' "$transcript" | tail -1 || true)"

  local outcome=PASS vanished=0 drift=0 thin=0
  [ "$n_fail" -gt 0 ] && outcome=FAIL
  # A run that died without printing a FAIL: line is the whole of #180's first three sightings. It is a
  # failure, and the verdict has to name what vanished rather than shrug.
  if [ "$child_rc" -ne 0 ] && [ "$n_fail" -eq 0 ]; then outcome=FAIL; vanished=1; fi
  # The other direction: evidence of failure with a zero status. The lines win — a counter that disagrees with
  # what was printed is the bug, not the verdict.
  if [ "$child_rc" -eq 0 ] && [ "$n_fail" -gt 0 ]; then drift=1; fi
  if [ "$outcome" = PASS ] && [ "$n_pass" -lt "$VERDICT_PASS_FLOOR" ]; then outcome=FAIL; thin=1; fi

  # The names, for the LAST line: a caller who only ever sees `tail -1` still learns which probe it was.
  local names
  names="$(printf '%s\n' "$fails" \
    | sed -E 's/^(FAIL|FATAL): +//; s/ \(.*$//; s/ - .*$//; s/ — .*$//; s/[[:space:]]+$//' \
    | awk 'NF && !seen[$0]++ { n++; if (n <= 6) s = (n == 1 ? $0 : s ", " $0) }
           END { if (n > 6) s = s " (+" (n - 6) " more)"; print s }')"
  if [ "$vanished" -eq 1 ]; then
    local dead
    dead="$(printf '%s' "${last_section:-}" | sed -e 's/^=== //' -e 's/ ===$//')"
    [ -n "$dead" ] || dead="the run"
    names="$dead (vanished: rc=$child_rc)"
  fi
  [ -n "$names" ] || names="the run produced too little output to be a pass"

  # The verdict file, written BEFORE anything is printed so the printed path cannot promise a file that was
  # never created. The wrapper deletes any previous one before the run, so a stale file cannot be read as
  # this run's result.
  local vf_note="$verdict_file"
  local exit_status=1; [ "$outcome" = PASS ] && exit_status=0
  if ! {
        echo "VERDICT=$outcome"
        echo "EXIT=$exit_status"
        echo "PASSED=$n_pass"
        echo "FAILED=$n_fail"
        echo "RUN_EXIT=$child_rc"
        echo "TRANSCRIPT=$transcript"
        echo "WHEN=$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || true)"
        [ "$outcome" = PASS ] || printf '%s\n' "$fails" | sed -e '/^$/d' -e 's/^/FAILURE=/'
        [ "$vanished" -eq 0 ] || echo "FAILURE=$names"
      } > "$verdict_file" 2>/dev/null; then
    vf_note="COULD NOT BE WRITTEN at $verdict_file"
  fi

  local vtmp; vtmp="$(mktemp)"
  {
    echo "=== verdict ==="
    if [ "$outcome" = PASS ]; then
      echo "$n_pass check(s) printed PASS, none printed FAIL or FATAL, and the run exited $child_rc."
      echo "verdict file: $vf_note"
      echo "full output:  $transcript"
      echo "ALL HEADLESS PROBES PASSED"
    else
      echo "Derived from what the run PRINTED (not from a counter kept beside it) - see issue #180."
      if [ "$n_fail" -gt 0 ]; then
        echo "$n_fail of $((n_pass + n_fail)) reported check(s) FAILED:"
        printf '%s\n' "$fails" | sed -e '/^$/d' -e 's/^/  - /'
      fi
      if [ "$vanished" -eq 1 ]; then
        echo "The run exited $child_rc$(exit_status_note "$child_rc" "$([ "$n_pass" -gt 0 ] && echo 1 || echo 0)") WITHOUT printing a single FAIL: line, so"
        echo "something died before it could report on itself. The last section it announced was:"
        echo "  ${last_section:-(it never announced one)}"
        echo "and the run's last lines were:"
        tail -6 "$transcript" | sed 's/^/  | /'
      fi
      if [ "$drift" -eq 1 ]; then
        echo "NOTE: the run's own exit status was 0 while $n_fail FAIL:/FATAL: line(s) were printed. The"
        echo "  verdict follows the LINES: a summary that can disagree with the evidence is issue #180 itself."
      fi
      if [ "$thin" -eq 1 ]; then
        echo "Only $n_pass PASS: line(s) came back, under the floor of $VERDICT_PASS_FLOOR. A run this short"
        echo "  did not assert what this suite asserts; do not read its silence as a pass."
      fi
      echo "verdict file: $vf_note"
      echo "full output:  $transcript"
      echo "SOME HEADLESS PROBES FAILED: $names"
    fi
  } > "$vtmp"

  # To stdout, appended to the transcript (so the log carries its own verdict) - and, when it is a failure,
  # to STDERR as well. A caller who pipes stdout into `tail`/`head`/a file still gets the failure on the
  # terminal, which is the half of the exit-status hazard the suite can actually do something about.
  tee -a "$transcript" < "$vtmp"
  [ "$outcome" = PASS ] || cat "$vtmp" >&2
  rm -f "$vtmp"
  return "$exit_status"
}
# ---- VERDICT BLOCK END -------------------------------------------------------------------------------------

# One process runs, another reports (issue #180). The suite re-executes itself once with stdout AND stderr
# through `tee` into a transcript, and the parent derives the verdict from that file after `tee` has exited -
# so there is no race with a background flush, and no output is withheld while the run is in progress. The
# child prints evidence and nothing else; it never prints a verdict.
#
# The point of the split is the case no in-process bookkeeping can cover: a run that dies where nothing can
# report on it - a `set -u` abort, an early FATAL exit, a shell killed outright - still gets a verdict, and
# that verdict names the last section the run announced and how it died.
if [ -z "${EB_PROBE_SUITE_CHILD:-}" ]; then
  EB_PROBE_LOG="${EB_PROBE_LOG:-$BUILD_DIR/headless-probes.log}"
  mkdir -p "$(dirname "$EB_PROBE_LOG")" 2>/dev/null || true
  : > "$EB_PROBE_LOG" 2>/dev/null || EB_PROBE_LOG="$(mktemp -t eb-probe-run.XXXXXX)"
  EB_PROBE_VERDICT="${EB_PROBE_VERDICT:-$BUILD_DIR/headless-probes.verdict}"
  rm -f "$EB_PROBE_VERDICT" 2>/dev/null || true   # a previous run's verdict is not this run's
  export EB_PROBE_LOG EB_PROBE_VERDICT
  EB_PROBE_SUITE_CHILD=1 "${BASH:-bash}" "${BASH_SOURCE[0]}" "$@" 2>&1 | tee "$EB_PROBE_LOG"
  eb_run_rc="${PIPESTATUS[0]}"
  suite_verdict "$EB_PROBE_LOG" "$eb_run_rc"
  exit "$?"
fi

# The suite owns the probes' data-dir configuration; the two hand-run escape hatches must not survive into it
# (issue #42). EB_PROBE_DATA_DIR pins every probe at ONE directory that AppPaths never cleans up (owned=false),
# so a single forgotten `export` in a developer's profile silently restores the exact collision this whole
# scheme exists to kill — state accreting across probes AND across runs — and nothing notices: probe_isolation
# scrubs the pin from its own child's environment and compares against the exe folder, so it passes anyway.
# EB_PROBE_DATA_DIR_KEEP is the same shape one step down: it disables cleanup, and the suite would leave a
# directory per probe per run behind. Both stay fully usable for running a probe by hand — this only says the
# suite starts from a known state.
unset EB_PROBE_DATA_DIR EB_PROBE_DATA_DIR_KEEP

# Same reasoning, one channel over (issue #180). EB_UITEST=1 in a developer's environment is routine now that
# increments are driven live, and it makes any probe that reaches UiTestServer::wanted() stand a control
# channel up on the DEFAULT pipe name - the one the live app being driven already holds. That is contention
# this suite has no business inheriting, and its symptom is a probe failing on a listen for reasons that have
# nothing to do with the code under test. probe_uitest names its own private channel regardless (see
# native/tools/probe_uitest.cpp); this covers everything else in the run.
unset EB_UITEST EB_UITEST_PIPE

# A probe exe may land at build/<name>, build/<name>.exe, or build/Release/<name>[.exe] (multi-config generators).
findexe() {
  local n="$1" p
  for p in "$BUILD_DIR/$n" "$BUILD_DIR/$n.exe" "$BUILD_DIR/Release/$n" "$BUILD_DIR/Release/$n.exe"; do
    [ -x "$p" ] && { echo "$p"; return 0; }
  done
  return 1
}

# ---- Stale-binary gate -------------------------------------------------------------------------------------
# This suite only ever EXECUTES pre-built binaries, so "the probe passed" means no more than "the binary
# sitting in $BUILD_DIR passed". When a probe stops compiling, the PREVIOUS build's exe is still there — and it
# runs, prints its sentinel, and passes, reporting on source that no longer exists. The missing-binary check in
# the probe loop below cannot see this one, because nothing is missing.
#
# Not hypothetical: it is what a812cfd found. A brace dropped in the #34 merge resolution took
# probe_cloudmerge out of the build, and the suite AND a direct run both reported CLOUDMERGE-OK — from a
# binary built before the merge. The fix at the time was procedural ("rebuild everything and grep the build
# output for errors"); this is that rule made mechanical.
#
# CI is not exposed — it configures a fresh build dir, so a compile error fails the build STEP and the suite
# never starts. A local incremental run is exposed, and a local run is where that merge was made.
#
# Source of truth is each probe's own add_executable() list in native/CMakeLists.txt: the files that actually
# compile into it. Deliberately NOT "anything under src/ is newer than the exe" — an unrelated edit does not
# relink this probe, so a tree-wide comparison would go red and STAY red however many times you rebuilt, which
# is the kind of gate people learn to skip. Scoped per target, every failure here is cleared by exactly the
# rebuild the message asks for.
#
# Known gap, deliberate: a header a probe #includes but that its add_executable() list does not name (AppPaths.h
# and AppBrand.h are the common ones) is invisible here. The build system tracks those properly; this gate is a
# backstop for the case where the build was not run at all, so it errs toward staying quiet rather than toward
# a failure a rebuild cannot clear. Same for probe_themeview's generated ${EB_THEME2_RCC_THEMEVIEW} input.
NATIVE_DIR="$(cd "$HERE/.." && pwd)"
# "<target>\t<path relative to native/>", one per line, for every add_executable(probe_*) in the file. Sources
# may run to the closing paren over many lines, and carry trailing # comments; both are handled. Tokens with no
# dot (the target name itself) and ${...}/$<...> (a generated input, not a file on disk yet) are skipped.
PROBE_SRCS="$(awk '
  { line = $0; sub(/#.*$/, "", line) }
  !on {
    if (!match(line, /add_executable\([ \t]*probe_[A-Za-z0-9_]+/)) next
    seg = substr(line, RSTART, RLENGTH); sub(/add_executable\([ \t]*/, "", seg); tgt = seg
    line = substr(line, RSTART + RLENGTH); on = 1
  }
  { if (!on) next
    if ((p = index(line, ")")) > 0) { line = substr(line, 1, p - 1); closing = 1 }
    n = split(line, toks, /[ \t]+/)
    for (i = 1; i <= n; i++) if (toks[i] ~ /\./ && toks[i] !~ /^\$/) print tgt "\t" toks[i]
    if (closing) { on = 0; closing = 0 } }
' "$NATIVE_DIR/CMakeLists.txt" 2>/dev/null)"
# The corpus guard the neighbouring gates have: a parser that silently stops matching would make every probe
# below "fresh" and this gate would announce nothing at all. The floor is deliberately far below the real count
# (52 at the time of writing, and it only grows) and far above zero, so it catches a moved CMakeLists, a changed
# declaration style, and a broken awk — without needing an edit every time a probe is added.
PROBE_SRC_TARGETS="$(printf '%s\n' "$PROBE_SRCS" | cut -f1 | sort -u | grep -c '[^[:space:]]' || true)"

# Every declared source of <target> that is NEWER than <exe>, one per line.
stale_sources() { # <target> <exe>
  local tgt="$1" exe="$2" f
  printf '%s\n' "$PROBE_SRCS" | awk -F'\t' -v t="$tgt" '$1 == t { print $2 }' | while IFS= read -r f; do
    [ -n "$f" ] && [ -e "$NATIVE_DIR/$f" ] || continue
    [ "$NATIVE_DIR/$f" -nt "$exe" ] && printf '%s\n' "$f"
  done
  return 0
}

# One scratch root per suite run (issue #42). Every probe binary is compiled with EB_ISOLATED_DATA_DIR, which
# points AppPaths::dataDir() at a per-PROCESS directory created under this root instead of at the exe's own
# folder — so a GUI run, a throwaway app in build/Release, and forty-odd probes stop sharing one
# everythingbox.ini. Each probe removes its own directory on exit; owning the ROOT here is what makes cleanup
# hold for a probe that CRASHES before its destructor runs, and it does so without a shared sweep that could
# delete a concurrent suite run's directories.
EB_PROBE_SCRATCH_ROOT_POSIX="$(mktemp -d "${TMPDIR:-/tmp}/eb-probe-scratch.XXXXXX")"
EB_PROBE_SCRATCH_ROOT="$EB_PROBE_SCRATCH_ROOT_POSIX"
# The probes are native binaries: on Windows (git-bash/MSYS) a POSIX /tmp/... path would be read by Qt as a
# path on the current drive's root, so hand them the Windows spelling of the same directory.
command -v cygpath >/dev/null 2>&1 && EB_PROBE_SCRATCH_ROOT="$(cygpath -m "$EB_PROBE_SCRATCH_ROOT_POSIX")"
export EB_PROBE_SCRATCH_ROOT
trap 'rm -rf "$EB_PROBE_SCRATCH_ROOT_POSIX"' EXIT

fail=0

if [ "${PROBE_SRC_TARGETS:-0}" -lt 20 ]; then
  echo "FAIL: stale-binary gate parsed $PROBE_SRC_TARGETS probe target(s) from native/CMakeLists.txt — expected"
  echo "      one per add_executable(probe_*) in that file, which is dozens. It is asserting nothing about"
  echo "      staleness; treat every PASS below as covering whichever binary happens to be on disk, not the"
  echo "      source in this tree."
  echo
  fail=1
fi

run() { # <name> <sentinel> <exe> [args...]
  local name="$1" sentinel="$2" exe="$3"; shift 2
  # Freshness first: a stale binary's sentinel is a report on the PREVIOUS build, so running it at all would
  # print a PASS-shaped line for source that was never compiled. The target name is the exe's basename —
  # run()'s $1 is prose for some call sites ("nav kit", "meta cache"), the exe path never is.
  local tgt stale
  tgt="$(basename "$exe")"; tgt="${tgt%.exe}"
  stale="$(stale_sources "$tgt" "$exe")"
  if [ -n "$stale" ]; then
    echo "=== $name ==="
    printf '%s\n' "$stale" | sed 's|^|    |'
    echo "FAIL: $name ($tgt was not rebuilt — the source(s) above are NEWER than its binary, so anything it"
    echo "      printed would describe the previous build. Rebuild the probe targets, then re-run.)"
    fail=1
    echo
    return
  fi
  echo "=== $name ==="
  local out rc seen said
  out="$("$@" 2>&1)"; rc=$?
  echo "$out"
  # A shell-native match, not `printf ... | grep -q`: grep -q exits the instant it matches, the printf takes
  # SIGPIPE, and under `set -o pipefail` the pipeline then reports 141 - i.e. FAILURE ON A MATCH - for any
  # probe verbose enough to fill the pipe buffer. Two gates further down hit that and worked around it with a
  # temp file; `case` needs neither a pipe nor a file. ("$sentinel" is quoted, so it is matched literally.)
  case "$out" in *"$sentinel"*) seen=1 ;; *) seen=0 ;; esac
  if [ "$rc" -eq 0 ] && [ "$seen" -eq 1 ]; then
    echo "PASS: $name"
  else
    # Say HOW, not just that. A probe killed by a signal never reached its own reporting, and three sightings
    # on issue #180 are what that looks like when nothing says so out loud.
    local printed=1; [ -n "$out" ] || printed=0
    if [ "$printed" -eq 0 ]; then said="it printed nothing at all"
    else said="it printed $(printf '%s\n' "$out" | wc -l | tr -d '[:space:]') line(s)"; fi
    if [ "$rc" -ne 0 ]; then
      echo "FAIL: $name (rc=$rc$(exit_status_note "$rc" "$printed"), $said, expected '$sentinel')"
    else
      echo "FAIL: $name (rc=0 but '$sentinel' is nowhere in its output, $said)"
    fi
    fail=1
  fi
  echo
}

# Bring up the relay both netplay tests rendezvous through.
RELAY_LOG="$(mktemp -t eb-relay.XXXXXX)"
"$PY" "$RELAY_PY" --port "$RELAY_PORT" > "$RELAY_LOG" 2>&1 &
RELAY_PID=$!
trap 'rm -rf "$EB_PROBE_SCRATCH_ROOT_POSIX"; [ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null; rm -f "${RELAY_LOG:-}"' EXIT
for _ in $(seq 1 100); do grep -q "listening" "$RELAY_LOG" 2>/dev/null && break; sleep 0.2; done
# The port the relay actually bound. Waiting for "listening" and then carrying on regardless is how a relay that
# never came up got papered over: the netplay probes would connect to SOMEONE's relay and the result meant
# nothing. No port here is a hard failure.
RELAY_PORT="$(sed -n 's/.*listening on [^:]*:\([0-9][0-9]*\).*/\1/p' "$RELAY_LOG" 2>/dev/null | head -1)"
if [ -z "$RELAY_PORT" ]; then
  echo "FATAL: the netplay relay did not come up — $(head -3 "$RELAY_LOG" 2>/dev/null)"; exit 2
fi
echo "relay: $(head -1 "$RELAY_LOG" 2>/dev/null)"; echo

NETPLAY="$(findexe probe_netplay)"       || { echo "FATAL: probe_netplay not built"; exit 2; }
BOTH="$(findexe probe_netplay_both)"     || { echo "FATAL: probe_netplay_both not built"; exit 2; }
NAV="$(findexe probe_nav)"               || { echo "FATAL: probe_nav not built"; exit 2; }
META="$(findexe probe_meta)"             || { echo "FATAL: probe_meta not built"; exit 2; }
ISO="$(findexe probe_isolation)"         || { echo "FATAL: probe_isolation not built"; exit 2; }

# The folder the probe binaries were built into — which on desktop is also what AppPaths::dataDir() used to
# resolve to, i.e. the app's whole data directory. Everything below that talks about "the exe folder" means
# this one.
EXE_DIR="$(cd "$(dirname "$ISO")" && pwd)"

# ---- App link gate (issue #182) ----------------------------------------------------------------------------
# Everything else in this suite EXECUTES a pre-built binary. Until this gate, NOTHING anywhere built the app,
# so `everythingbox` could be uncompilable and the suite still printed ALL HEADLESS PROBES PASSED. That is not
# hypothetical: #128 put src/core/RomPatch.cpp in probe_softpatch's source list but NOT in the app's, the app's
# GameLauncher::prepareCore called RomPatch::resolvePatchedRom, EverythingBox.exe died on LNK2019 -> LNK1120 —
# and this suite printed ALL HEADLESS PROBES PASSED *and* SOFTPATCH-OK. The register-in-three-places discipline
# is about PROBES; the app's own source list is a fourth place no checklist covered.
#
# CI was not the safety net the issue assumed either. .github/workflows/ci.yml configures with
# EVERYTHINGBOX_BUILD_APP=ON but its build step names ~120 probe_* targets and NOT everythingbox, so between
# releases nothing compiled the app at all; the only builds of it lived in release.yml, which until 2026-08 ran
# solely on a version tag and quietly accumulated four breakages. Same structural fault, twice: the thing that
# builds the shipped artifact is not the thing that runs on every push. Hence this gate is ON BY DEFAULT and
# lives in the suite rather than in a workflow step — the suite is what CI runs and what a worker reports.
echo "=== app link ==="
app_fail=0
APP_BINDIR=""
APP_CFG=""
APP_OUTDIR=""
# BUILD_DIR is the CMake *binary* dir for a single-config generator (build) but a CONFIG SUBDIR for a
# multi-config one (build/Release) — both spellings are documented at the top of this file — so accept either.
for _abd in "$BUILD_DIR" "$BUILD_DIR/.."; do
  if [ -f "$_abd/CMakeCache.txt" ]; then APP_BINDIR="$(cd "$_abd" && pwd)"; break; fi
done
if [ -z "$APP_BINDIR" ]; then
  echo "FAIL: APP LINK — no CMakeCache.txt at '$BUILD_DIR' or its parent, so the everythingbox target cannot be"
  echo "      built from here and NOTHING in this run checks that the app still compiles. Point BUILD_DIR at a"
  echo "      configured CMake build directory (BUILD_DIR=build, or BUILD_DIR=build/Release)."
  app_fail=1
elif ! command -v cmake >/dev/null 2>&1; then
  echo "FAIL: APP LINK — cmake is not on PATH, so the everythingbox target cannot be built and nothing in this"
  echo "      run checks that the app still compiles. This is a FAIL rather than a skip on purpose: a gate that"
  echo "      no-ops when its tool is missing is the hole issue #182 is about."
  app_fail=1
elif ! grep -q '^EVERYTHINGBOX_BUILD_APP:BOOL=ON' "$APP_BINDIR/CMakeCache.txt"; then
  echo "FAIL: APP LINK — $APP_BINDIR was configured with EVERYTHINGBOX_BUILD_APP=OFF, so there is no"
  echo "      everythingbox target to build. Every probe target lives inside that same CMake gate, so a build"
  echo "      dir that can run this suite is always configured with it ON; re-configure with"
  echo "      -DEVERYTHINGBOX_BUILD_APP=ON."
  app_fail=1
else
  # Multi-config generators need --config, and it has to be the SAME configuration the probes above came out
  # of, or this would prove a different binary than the one sitting beside them. $EXE_DIR is <bindir>/<config>
  # in that case; the ${EXE_DIR:-$BUILD_DIR} default is so this section still runs standalone under
  # native/tools/applink-gate-check.sh, which supplies only BUILD_DIR.
  if grep -q '^CMAKE_CONFIGURATION_TYPES:' "$APP_BINDIR/CMakeCache.txt"; then
    case "$(basename "${EXE_DIR:-$BUILD_DIR}")" in
      Debug|Release|RelWithDebInfo|MinSizeRel) APP_CFG="$(basename "${EXE_DIR:-$BUILD_DIR}")" ;;
      *) APP_CFG="Release" ;;
    esac
    APP_OUTDIR="$APP_BINDIR/$APP_CFG"
  else
    APP_OUTDIR="$APP_BINDIR"
  fi

  # Cost control, and the one judgement call in here. A NO-OP `cmake --build --target everythingbox` still
  # costs ~28s on the Windows/VS generator (its POST_BUILD windeployqt + core-staging commands re-run every
  # time) against a ~2m50s suite, and developers run this constantly. So the build is skipped only when the
  # exe on disk is provably newer than EVERY file the app is compiled from — scanned tree-wide over the app's
  # source roots, deliberately NOT the per-target list the stale-binary gate above uses.
  #
  # Tree-wide is the safe direction HERE, unlike there. The stale-binary gate's response to "out of date" is to
  # FAIL, so a tree-wide scan would go red and stay red however often you rebuilt; this gate's response is to
  # BUILD, which clears the condition. So the only cost of scanning too widely is one extra rebuild, and the
  # check can never skip a build that was needed. It also self-heals after a red: a failed link leaves the OLD
  # exe on disk while the sources stay newer than it, so the next run builds again and fails again.
  APP_EXE=""
  for _ax in "$APP_OUTDIR/EverythingBox.exe" "$APP_OUTDIR/EverythingBox" "$APP_OUTDIR/everythingbox.exe" "$APP_OUTDIR/everythingbox"; do
    [ -f "$_ax" ] && { APP_EXE="$_ax"; break; }
  done
  APP_NEED_BUILD=1
  APP_SKIP_WHY=""
  if [ -n "$APP_EXE" ]; then
    # src = the app's code; resources/third_party/themes2 = the .qrc, miniz/duktape and the themed QML that
    # compile into it; CMakeLists.txt = the source LIST itself, which is where #128's omission actually lived.
    APP_SRC_N=0
    for _ar in src resources third_party themes2; do
      [ -d "$NATIVE_DIR/$_ar" ] && APP_SRC_N=$(( APP_SRC_N + $(find "$NATIVE_DIR/$_ar" -type f 2>/dev/null | wc -l) ))
    done
    # The corpus guard its neighbours have: a find that silently matched nothing would report "up to date"
    # forever and this gate would never build anything again. The floor is far below the real count (~620) and
    # far above zero. Below it, build anyway rather than trust the scan.
    if [ "$APP_SRC_N" -lt 200 ]; then
      echo "    (freshness scan found only $APP_SRC_N app source file(s) under $NATIVE_DIR — too few to trust, building unconditionally)"
    else
      APP_NEWER="$( { for _ar in src resources third_party themes2; do
                        [ -d "$NATIVE_DIR/$_ar" ] && find "$NATIVE_DIR/$_ar" -type f -newer "$APP_EXE" 2>/dev/null
                      done
                      [ "$NATIVE_DIR/CMakeLists.txt" -nt "$APP_EXE" ] && printf '%s\n' "$NATIVE_DIR/CMakeLists.txt"
                      true; } | head -5 )"
      if [ -z "$APP_NEWER" ]; then
        APP_NEED_BUILD=0
        APP_SKIP_WHY="$(basename "$APP_EXE") is newer than all $APP_SRC_N app source file(s) and native/CMakeLists.txt"
      fi
    fi
  fi

  if [ "$APP_NEED_BUILD" -eq 0 ]; then
    echo "PASS: app link (up to date, not rebuilt — $APP_SKIP_WHY)"
  else
    echo "    building target everythingbox in $APP_BINDIR${APP_CFG:+ [$APP_CFG]} ..."
    APP_BUILD_ARGS=(--build "$APP_BINDIR" --target everythingbox)
    [ -n "$APP_CFG" ] && APP_BUILD_ARGS+=(--config "$APP_CFG")
    APP_LOG="$(mktemp -t eb-applink.XXXXXX)"
    if cmake "${APP_BUILD_ARGS[@]}" > "$APP_LOG" 2>&1; then
      app_rc=0
    else
      app_rc=$?
    fi
    if [ "$app_rc" -eq 0 ]; then
      echo "PASS: app link (everythingbox built)"
      rm -f "$APP_LOG"
    else
      # Name the failure, and name it as the APP's (issue #180: a gate that reports failure without saying what
      # failed teaches people to re-run rather than investigate). Every PASS above this line is a pre-built
      # PROBE binary; none of them link the app, so none of them are what broke.
      APP_ERRS="$(grep -nE 'LNK[0-9]{4}|error [A-Z]+[0-9]+|undefined reference|cannot find -l|No rule to make target|ninja: error|MSB[0-9]{4}|unknown target' "$APP_LOG" | head -25 || true)"
      [ -n "$APP_ERRS" ] && printf '%s\n' "$APP_ERRS" | sed 's|^|    |'
      echo "    ---- last 20 lines of $APP_LOG ----"
      tail -20 "$APP_LOG" | sed 's|^|    |'
      echo "FAIL: APP LINK — the everythingbox APP TARGET DID NOT BUILD (cmake --build exited $app_rc)."
      echo "      This is NOT a probe failure. Every PASS above is a pre-built probe binary and none of them"
      echo "      link the app, so re-running this suite will not change anything: EverythingBox.exe cannot be"
      echo "      produced from this tree. The usual cause is a source the app calls into that is registered on"
      echo "      a probe target but missing from qt_add_executable(everythingbox ...) in native/CMakeLists.txt"
      echo "      — that is exactly #128, and it is a FOURTH registration site the probe checklist does not"
      echo "      cover. Full build log kept at $APP_LOG."
      app_fail=1
    fi
  fi
fi
[ "$app_fail" -eq 0 ] || fail=1
echo

# A fingerprint of the app-data footprint inside the exe folder. Compared before and after the suite by the
# "exe-folder contamination" gate at the bottom: no probe may create, modify or delete anything the app reads
# there. Top-level entries catch a file or directory APPEARING (the common shape — everythingbox.ini,
# addons/, metadata/, themes/, saves-meta.json, stream_debug.log all used to land here); the recursive walk
# and the checksums catch a write INTO something that was already present.
exe_fingerprint() {
  ( cd "$EXE_DIR" 2>/dev/null && ls -A . | sort )
  ( cd "$EXE_DIR" 2>/dev/null && for d in addons metadata themes themes2 saves states downloads cores emulators music; do
      [ -d "$d" ] && find "$d" -type f | sort
    done )
  ( cd "$EXE_DIR" 2>/dev/null && for f in everythingbox.ini mymediavault.ini saves-meta.json; do
      [ -f "$f" ] && cksum "$f"
    done )
  return 0
}

# ---- Probe data-dir isolation (issue #42) ------------------------------------------------------------------
# The reproduction, made permanent. Seed the exe folder with exactly what an issue-#42 collision looks like —
# a throwaway everythingbox.ini carrying a sentinel key, and an add-on folder — then run the probe that
# asserts it can see NEITHER, that its own writes leave that folder byte-identical, and that a second probe
# process gets a different directory which disappears with the process. Before the fix, a suite run with junk
# in build/Release failed for reasons that had nothing to do with the branch under test; this step is what
# stops that from coming back silently.
#
# The snapshot is taken BEFORE the seeding, not after: this step has to sit INSIDE the contamination gate's
# window like every other probe, or a probe writing into the exe folder from here would be baked into the
# "before" picture and go unnoticed. The seed/restore below is exact, so the gate sees it as a no-op — and
# checks that the restore really was exact.
EXE_FP_BEFORE="$(exe_fingerprint)"

echo "=== probe data-dir isolation (seeding the exe folder with junk first) ==="
ISO_JUNK_INI="$EXE_DIR/everythingbox.ini"
ISO_JUNK_ADDON="$EXE_DIR/addons/probeisolationjunk"
ISO_INI_BAK=""
ISO_ADDONS_ROOT_MADE=0
[ -e "$ISO_JUNK_INI" ] && { ISO_INI_BAK="$(mktemp)"; cp "$ISO_JUNK_INI" "$ISO_INI_BAK"; }
[ -d "$EXE_DIR/addons" ] || ISO_ADDONS_ROOT_MADE=1
printf '\n[probeIsolation]\nsentinel=EXE-DIR-JUNK\n' >> "$ISO_JUNK_INI"
mkdir -p "$ISO_JUNK_ADDON"
printf '%s\n' '{ "id":"com.everythingbox.probeisolationjunk","name":"Junk","version":"1.0.0","type":"media-source","entry":"main.js","permissions":[],"catalogs":[] }' > "$ISO_JUNK_ADDON/manifest.json"
printf 'function getMeta(a){ return "{}"; }\n' > "$ISO_JUNK_ADDON/main.js"
run "probe_isolation" ISOLATION-OK "$ISO"
# Restore, unconditionally — a failing probe must not leave the junk behind either.
if [ -n "$ISO_INI_BAK" ]; then cp "$ISO_INI_BAK" "$ISO_JUNK_INI"; rm -f "$ISO_INI_BAK"; else rm -f "$ISO_JUNK_INI"; fi
rm -rf "$ISO_JUNK_ADDON"
[ "$ISO_ADDONS_ROOT_MADE" = 1 ] && rmdir "$EXE_DIR/addons" 2>/dev/null

run "netplay relay"       NETPLAY-RELAY-OK "$NETPLAY" "$RELAY_PORT"
run "netplay both:direct" NETPLAY-BOTH-OK  "$BOTH" direct
run "netplay both:relay"  NETPLAY-BOTH-OK  "$BOTH" relay "$RELAY_PORT"
# The joiner's give-up deadline measures the HANDSHAKE, not the connect plus the handshake — and a session the
# user left inside that window stays left. Deliberately slow (~5s): the only way to tell the two readings of
# that deadline apart is to make the connect itself cost most of the budget, which on loopback means putting a
# stalling CONNECT proxy in front of it.
run "netplay both:slowconnect" NETPLAY-BOTH-OK "$BOTH" slowconnect
# The two ways a direct endpoint can accept a connection and still be a dead end — a stale port forward that
# outlived its app, and an EB host that already paired with somebody else. Both used to strand the joiner,
# because the fallback was keyed on the TCP connect rather than on the handshake completing.
run "netplay both:silent"  NETPLAY-BOTH-OK "$BOTH" silent  "$RELAY_PORT"
run "netplay both:dropped" NETPLAY-BOTH-OK "$BOTH" dropped "$RELAY_PORT"

# Controller-navigation invariants (offscreen QPA): a selection always exists, arrows clamp + recover from
# deleted rows, overlays stack/unwind and restore focus, Back always routes, the on-screen keyboard works.
run "nav kit"             NAV-OK           "$NAV" -platform offscreen

# Offline metadata cache: item/detail round-trips, merge preserves unknown (future) keys, cached artwork
# wins over remote urls, uninstall removes the bundle.
run "meta cache"          META-OK          "$META"

# Extensible artwork/videos/audio/metadata schema (MediaArt): provider JSON parse (+ role synonyms), the
# themed item-map bindings (scalar aliases + galleries), the game aggregator's role-precedence merge, and
# offline round-tripping with cached-file-first resolution. Same probe binary as the meta cache.
run "media-art schema"    ART-OK           "$META"

# Per-item metadata overrides (issue #24): the correction a user makes when the scraper got an item wrong.
# The record's one canonical spelling (trimmed, omit-empty — so two devices' identical corrections are
# identical BYTES), the override-beats-scraped composite, that all three MetaCache read primitives run it,
# that a re-scrape cannot discard it, and that reset restores the scraped values. Same probe binary again.
# The cross-device half is probe_cloudmerge section 20.
run "metadata overrides"  OVERRIDE-OK      "$META"

# Addon engine + manager: builtinCredential scoping, catalog cache hit/miss, the prefetcher's in-flight cap,
# reload-mid-sweep recovery, the TTL/watchdog paths, and the reserved-namespace install guard. Self-contained
# — it writes its own JsLocal fixtures into a temp EB_ADDONS_ROOT and touches no network. The `--prefetch`
# mode is the ASSERTING one (ADDON-OK); probe_addon's other modes take a real addon script or a live URL.
#
# This target existed and was maintained for a long time WITHOUT being wired into this script or CI, so every
# assertion in it gated nothing. Adding a probe target is not the same as running it — if you add one, add it
# here too.
ADDON="$(findexe probe_addon)"           || { echo "FATAL: probe_addon not built"; exit 2; }
run "addon engine+manager" ADDON-OK      "$ADDON" --prefetch

# EmulationStation / RetroBat gamelist.xml reader + write-back: parse a real gamelist, match a ROM, resolve
# ES media roles to local files, and round-trip a write. Passes trivially where there's no RetroBat data
# (CI), verifies for real where C:\RetroBat exists. Optional: only if built.
GAMELIST="$(findexe probe_gamelist || true)"
[ -n "$GAMELIST" ] && run "gamelist (ES/RetroBat)" GAMELIST-OK "$GAMELIST" \
  || echo "(skip) probe_gamelist not built"

# Queued game-metadata aggregator: entering a console prefetches + caches all its games (throttled), a hover
# scrapes at priority, every result is cached (scroll-past never drops a scrape), cached games aren't
# re-scraped. Uses a canned keyless provider in the build tree (no API keys). Optional: only if built.
GAMEAGG="$(findexe probe_gameagg || true)"
[ -n "$GAMEAGG" ] && run "game meta aggregator" GAMEAGG-OK "$GAMEAGG" \
  || echo "(skip) probe_gameagg not built"

# Themed video: MpvPreview (libmpv software-render) decodes mpv's built-in lavfi test source and paints real
# frames into a Qt Quick software-backend scene — the RetroBat-style in-menu playback path. Optional: only
# runs where the QML build (+ libmpv) produced the probe.
MPVPREV="$(findexe probe_mpvpreview || true)"
[ -n "$MPVPREV" ] && run "mpv video preview" MPV-PREVIEW-OK "$MPVPREV" -platform offscreen \
  || echo "(skip) probe_mpvpreview not built"

# Optional: prove the libretro frontend can load a real core headlessly. Best-effort — a missing/incompatible
# core is a warning, not a CI failure (the core comes from an external buildbot we don't control).
if [ -n "${CORE_SO:-}" ] && [ -f "$CORE_SO" ]; then
  CORE="$(findexe probe_core || true)"
  if [ -n "$CORE" ]; then
    echo "=== core load: $(basename "$CORE_SO") ==="
    if "$CORE" "$CORE_SO"; then echo "PASS (advisory): core loaded + retro_init ran"
    else echo "WARN: core-load probe failed (advisory, not gating CI)"; fi
    echo
  fi
else
  echo "note: no \$CORE_SO provided — skipping the libretro core-load probe"; echo
fi

# Foundation-refactor seams: Notifier (window/player notice channel), StreamResolver's m3u/stream
# classification, PlaybackSession (audio queue + resume state machine), and the synthetic browse
# catalogs (Recent/Downloaded/Favorites builders) — each extracted pure and probe-tested.
for p in "probe_navqml NAVQML-OK" "probe_themeview THEMEVIEW-OK" "probe_notifier NOTIFIER-OK" "probe_m3u M3U-OK" "probe_discgroup DISCGROUP-OK" "probe_regioncollapse REGIONCOLLAPSE-OK" "probe_playback PLAYBACK-OK" "probe_queueedit QUEUEEDIT-OK" "probe_bgaudio BGAUDIO-OK" "probe_browse BROWSE-OK" "probe_leafroute LEAFROUTE-OK" "probe_perf PERF-OK" "probe_formfactor FORMFACTOR-OK" "probe_bootstrap BOOTSTRAP-OK" "probe_sync SYNC-OK" "probe_extplayer EXTPLAYER-OK" "probe_marks MARKS-OK" "probe_bookmarks BOOKMARKS-OK" "probe_audiobookmarks AUDIOBM-OK" "probe_opds OPDS-OK" "probe_tar TAR-OK" "probe_launchopts LAUNCHOPTS-OK" "probe_emutargets EMUTARGETS-OK" "probe_emulation_scope probe_emulation_scope:" "probe_pcscan PCSCAN-OK" "probe_emusettings EMUSETTINGS-OK" "probe_shaderpreset SHADERPRESET-OK" "probe_librashader LIBRASHADER-OK" "probe_shaderchain SHADERCHAIN-OK" "probe_shaderassets SHADERASSETS-OK" "probe_deviceprofile DEVICEPROFILE-OK" "probe_pad2key PAD2KEY-OK" "probe_seats SEATS-OK" "probe_launchhooks LAUNCHHOOKS-OK" "probe_filterpreset FILTERPRESET-OK" "probe_hwdecode HWDECODE-OK" "probe_hardcore HARDCORE-OK" "probe_substyle SUBSTYLE-OK" "probe_readertypography READERTYPO-OK" "probe_refreshsync REFRESHSYNC-OK" "probe_hdroutput HDROUTPUT-OK" "probe_replaygain REPLAYGAIN-OK" "probe_crossfade CROSSFADE-OK" "probe_audioout AUDIOOUT-OK" "probe_contentlang CONTENTLANG-OK" "probe_softpatch SOFTPATCH-OK" "probe_romhack ROMHACK-OK" "probe_homebrew HOMEBREW-OK" "probe_overrides OVERRIDES-OK" "probe_hashverify HASHVERIFY-OK" "probe_stats STATS-OK" "probe_playlists PLAYLISTS-OK" "probe_photos PHOTOS-OK" "probe_iptv IPTV-OK" "probe_xmltv XMLTV-OK" "probe_cloudmerge CLOUDMERGE-OK" "probe_importers IMPORTERS-OK" "probe_onboarding ONBOARDING-OK" "probe_locallib LOCALLIB-OK" "probe_resolver RESOLVER-OK" "probe_showdispatch SHOWDISPATCH-OK" "probe_docbridge DOCBRIDGE-OK" "probe_subs SUBS-OK" "probe_segments SEGMENTS-OK" "probe_listening LISTENING-OK" "probe_lyrics LYRICS-OK" "probe_lyricseek LYRICSEEK-OK" "probe_lyricsources LYRICSOURCES-OK" "probe_cuesheet CUE-OK" "probe_musictags MUSICTAGS-OK" "probe_musiclibrary MUSICLIB-OK" "probe_musicbrowse MUSICBROWSE-OK" "probe_musicqueue MUSICQUEUE-OK" "probe_audiobooks AUDIOBOOKS-OK" "probe_books BOOKS-OK" "probe_stremio STREMIO-OK" "probe_savesync SAVESYNC-OK" "probe_serversync SERVERSYNC-OK" "probe_brand BRAND-OK" "probe_theme THEME-OK" "probe_settingstxn SETTINGSTXN-OK" "probe_trakt TRAKT-OK" "probe_scrobble SCROBBLE-OK" "probe_subsonic SUBSONIC-OK" "probe_musicid MUSICID-OK" "probe_musicremap MUSICREMAP-OK" "probe_displaytitle DISPLAYTITLE-OK" "probe_passcode PASSCODE-OK" "probe_pcgames PCGAMES-OK" "probe_crashreport CRASHREPORT-OK" "probe_uitest UITEST-OK" "probe_themereg THEMEREG-OK" "probe_miximage MIXIMAGE-OK" "probe_attract ATTRACT-OK" "probe_manual MANUAL-OK" "probe_stateslots STATESLOTS-OK" "probe_bezel BEZEL-OK" "probe_cheatsearch CHEATSEARCH-OK" "probe_remoteapi REMOTEAPI-OK" "probe_syscatalog SYSCATALOG-OK" "probe_romrouting ROMROUTING-OK" "probe_archiverom ARCHIVEROM-OK" "probe_useremulators USEREMU-OK" "probe_bulkselect BULKSELECT-OK" "probe_comicfit COMICFIT-OK" "probe_retropark_input RETROPARK-INPUT-OK" "probe_coreopts COREOPTS-OK" "probe_ps3update PS3UPDATE-OK" "probe_ps3firmware PS3FIRMWARE-OK" "probe_launchcancel LAUNCHCANCEL-OK" "probe_launchcontexts LAUNCHCONTEXTS-OK"; do
  set -- $p
  # A probe in THIS list is not optional. If its binary is missing the probe did not pass -- it did not
  # run, and the commonest cause is that it stopped COMPILING. Treating that as a skip is how a broken
  # probe leaves the gate silently: the suite only ever executes pre-built binaries, so a compile
  # failure and a deleted assertion look identical from here. It has happened -- a dropped brace in a
  # merge took probe_cloudmerge out of the suite for two full runs while both reported success.
  # The genuinely optional probes (mpv/gamelist/gameagg/$CORE_SO) are handled above, by name.
  if exe=$(findexe "$1"); then run "$1" "$2" "$exe"; else
    echo "FAIL: $1 is not built — it cannot have passed. Build it (a compile error is the usual cause)."
    fail=1
  fi
done

# QML no-direct-selection-writes gate (B2 Task 6): NavGraph is the SINGLE source of truth for the themed
# surface's selection — the QML routes every arrow/click through nav.move()/nav.select() and the C++ bridge
# mirrors the RESOLVED selection back into the props the theme binds. A QML file that assigns one of those
# selection-state props directly (`currentIndex = …`) bypasses the model's clamp/divider-snap arbitration and
# desyncs the two, so any such write must FAIL the suite. Line-comments are stripped first (via sed) so prose
# that merely MENTIONS a prop ("the bridge writes focusZone=0") never trips the gate. Scans the whole theme2
# QML tree (ThemeView.qml + every element).
echo "=== qml no-direct-selection-writes ==="
QML_DIR="$HERE/../src/theme2/qml"
SEL_PROPS='currentIndex|catIndex|buttonIndex|actionIndex|focusZone|detailActionIndex|detailChildIndex|audioTransportIndex|audioQueueIndex'
qml_sel_hits=""
if [ -d "$QML_DIR" ]; then
  while IFS= read -r -d '' f; do
    hit="$(sed -E 's://.*$::' "$f" | grep -nE "($SEL_PROPS)[[:space:]]*=[^=]" || true)"
    [ -n "$hit" ] && qml_sel_hits="$qml_sel_hits"$'\n'"$f:"$'\n'"$hit"
  done < <(find "$QML_DIR" -name '*.qml' -print0)
fi
if [ -n "$qml_sel_hits" ]; then
  echo "$qml_sel_hits"
  echo "FAIL: qml no-direct-selection-writes (a QML file assigns a NavGraph-owned selection prop directly)"
  fail=1
else
  echo "PASS: qml no-direct-selection-writes"
fi
echo

# RetroView battery-save path gate (save-sync T4): where a .srm lives is decided in ONE place —
# SaveMeta::resolvePath, called from RetroView::sramPath() — because that function is the only thing that knows
# a save may live flat (pre-namespacing) or under a DIFFERENT system's namespace than the one this launch
# resolved. A second `".srm"` literal anywhere else in RetroView.cpp is a path built by hand, and a hand-built
# path misses both fallbacks: the game boots with an empty save while the real one sits on disk, unreachable.
# resolvePath itself is asserted by probe_savesync; this gate asserts nothing bypasses it. Line-comments are
# stripped first (via sed) so prose that merely mentions ".srm" never trips it.
echo "=== retroview srm-path gate ==="
RETROVIEW="$HERE/../src/emu/RetroView.cpp"
srm_hits=""
if [ -f "$RETROVIEW" ]; then
  srm_hits="$(sed -E 's://.*$::' "$RETROVIEW" | awk '
    /QString RetroView::sramPath/ { inpath = 1 }
    inpath && /^}/               { inpath = 0; next }
    /"\.srm"/ && !inpath         { print NR": "$0 }
  ')"
else
  echo "FAIL: retroview srm-path gate (RetroView.cpp not found at $RETROVIEW)"; fail=1
fi
if [ -n "$srm_hits" ]; then
  echo "$srm_hits"
  echo "FAIL: retroview srm-path gate (a \".srm\" path is built outside RetroView::sramPath)"
  fail=1
else
  echo "PASS: retroview srm-path gate"
fi
echo

# Crash-reporter handler-discipline gate (issue #28). Everything below the formatter in CrashReport.cpp sits
# inside `#ifdef _WIN32`, so CI COMPILES NONE OF IT — which is precisely how two rules the handler's whole value
# depends on came to be violated without a single assertion going red. Neither is expressible as a unit test
# (one is codegen, the other is what a Win32 call does internally), so the SOURCE SHAPE is gated here instead.
# Both failures are silent by construction: nobody reads crash_report.log until after the crash.
#
#   * appendLog must be WriteFile + FlushFileBuffers and nothing else. CreateFileW converts its DOS path to an
#     NT path through RtlDosPathNameToNtPathName_U, which ALLOCATES from the process default heap and takes the
#     heap lock. A per-append CreateFileW therefore puts an allocation on the handler's path — in a process
#     whose heap may be exactly what is broken (a near neighbour of #28's garbage-vector-entry shape), or whose
#     faulting thread is inside malloc/free holding that very lock. The handle is opened ONCE, in install().
#     The file I/O itself is the accepted trade; the per-append path conversion is not.
#
#   * Neither handler may declare a CrashRecord (~1.7 KB) or the scratch buffer (2.5 KB) in its OWN frame. A
#     prologue commits that frame through __chkstk, which probes it page by page, BEFORE any branch is taken —
#     and both handlers run for EXCEPTION_STACK_OVERFLOW, where roughly one page of stack is left and exception
#     dispatch has already spent part of it. A handler that paid that prologue would fault inside itself on
#     every stack-overflow crash: the process dies in the handler, g_prevFilter (WER) is never reached, and a
#     stack overflow that used to produce a WER dump produces nothing. So those locals live in
#     __declspec(noinline) helpers, and unhandledFilter tests faultMustSkipRecording() before anything else.
#     All three parts are load-bearing: the test alone still pays the prologue, and dropping `noinline` lets the
#     optimiser fold the frame straight back into the caller.
#
# The DECISION in that last test is pure and asserted properly, by probe_crashreport. This gate covers only what
# a probe cannot see.
echo "=== crashreport handler discipline ==="
CRCPP="$HERE/../src/core/CrashReport.cpp"
cr_fail=0
cr_note() { echo "  $1"; cr_fail=1; }
if [ ! -f "$CRCPP" ]; then
  echo "FAIL: crashreport handler discipline (CrashReport.cpp not found at $CRCPP)"; fail=1
else
  # Line-comments stripped first, so prose that merely NAMES CreateFileW or CrashRecord never trips the gate —
  # the comments in that file discuss both at length, which is the point of them.
  cr_src="$(sed -E 's://.*$::' "$CRCPP")"
  # One function body, from its definition line to the `    }` that closes it (everything here lives in an
  # anonymous namespace, so that indent is the function-level closing brace).
  cr_body() { printf '%s\n' "$cr_src" | awk -v sig="$1" '
    !on && index($0, sig) { on = 1 }
    on                    { print }
    on && /^    \}/       { exit }
  '; }

  cr_ap="$(cr_body 'void appendLog(const char* data, std::size_t len)')"
  if [ -z "$cr_ap" ]; then
    cr_note "appendLog not found — signature changed? This gate is now asserting nothing about it."
  else
    printf '%s' "$cr_ap" | grep -q 'CreateFile' \
      && cr_note "appendLog calls CreateFile*: the DOS->NT path conversion allocates from the default heap and takes the heap lock, on the handler's path. Open the handle once in install()."
    printf '%s' "$cr_ap" | grep -q 'WriteFile' \
      || cr_note "appendLog does not call WriteFile — nothing reaches the log."
    printf '%s' "$cr_ap" | grep -q 'FlushFileBuffers' \
      || cr_note "appendLog does not FlushFileBuffers: emitRecord's whole ordering discipline needs 'already written' to mean 'survives a hang in the next step'."
  fi

  for cr_h in 'LONG CALLBACK vectoredHandler(EXCEPTION_POINTERS* ep)' \
              'LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep)'; do
    cr_b="$(cr_body "$cr_h")"
    if [ -z "$cr_b" ]; then
      cr_note "handler not found: $cr_h — signature changed? This gate is now asserting nothing about it."
      continue
    fi
    printf '%s' "$cr_b" | grep -q 'CrashRecord' \
      && cr_note "$cr_h declares a CrashRecord (~1.7 KB) in its own frame: the prologue commits it on EVERY exception, including the stack overflow that then faults inside the handler and loses the WER dump. Move it to a __declspec(noinline) helper."
    printf '%s' "$cr_b" | grep -q 'kScratch' \
      && cr_note "$cr_h declares the scratch buffer (2.5 KB) in its own frame: same prologue, same lost WER dump. Move it to a __declspec(noinline) helper."
  done

  cr_uf="$(cr_body 'LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep)')"
  printf '%s' "$cr_uf" | grep -q 'faultMustSkipRecording' \
    || cr_note "unhandledFilter does not test faultMustSkipRecording: a stack overflow would walk into the record path instead of straight to the previous filter."
  printf '%s' "$cr_uf" | grep -q 'g_prevFilter' \
    || cr_note "unhandledFilter does not chain to g_prevFilter — WER stops getting the dump."

  # COUNTS, not `grep -q`: $cr_src is the WHOLE file, and this suite runs under `set -o pipefail`, so grep -q
  # exiting on the first match SIGPIPEs the printf that feeds it — a match then fails the pipeline — once the file
  # outgrows the pipe buffer. This matches cr_creates' grep -c just below. See the note above the metadata-editor gate.
  for cr_fn in writeFirstChanceRecord writeFatalRecord; do
    [ "$(printf '%s\n' "$cr_src" | grep -c "__declspec(noinline) void $cr_fn(" || true)" != "0" ] \
      || cr_note "$cr_fn is not __declspec(noinline): inlined back into its handler, its frame returns to the handler's prologue and the split buys nothing."
  done

  cr_creates="$(printf '%s\n' "$cr_src" | grep -c 'CreateFileW' || true)"
  [ "$cr_creates" = "1" ] \
    || cr_note "expected exactly one CreateFileW in CrashReport.cpp (install()'s); found $cr_creates."

  if [ "$cr_fail" -eq 0 ]; then echo "PASS: crashreport handler discipline"; else
    echo "FAIL: crashreport handler discipline (the Windows half violates a rule CI cannot compile)"; fail=1
  fi
fi
echo

# Themed-handler deferral gate (issue #28). MainWindow and HomeView link into NO probe — they need a QApplication,
# a QML engine, a theme on disk and an add-on manager — so the rule below cannot be asserted as behaviour at all.
# It is held here as source shape, on the crashreport-handler-discipline model.
#
# THE RULE. Every callback ThemeEngine::buildView is handed is a DIRECT connection from a QML signal, so it runs
# with a delegate's own emission on the stack. Re-sourcing a model or retiring a QQuickWidget from there only
# QUEUES the delegates' destruction (QQmlDelegateModel::release goes through deleteLater), which is why that has
# always been survivable. A NESTED EVENT LOOP between the two is what is not: NavMenu::pick, NavConfirm::ask,
# NavCountdown::ask, Osk::getText and PasscodePad::ask are all QEventLoop::exec, and a nested loop flushes those
# pending DeferredDeletes early, while the Repeater that owns them is still being walked — the
# ~QQuickItem -> itemChange -> QQuickRepeater::regenerate -> clear() chain both production dumps land in.
#
# So: a themed handler may not spin a nested loop on the emission's own stack. It defers a turn first.
#
# What this gate CANNOT see: whether a queued lambda captured a stable id or a row index. That half is a review
# obligation, written on deferPastQmlEmission's definition. What it CAN see is that the loop moved off the
# emission at all, which is the half that a later edit silently undoes.
echo "=== themed handler deferral ==="
MWCPP="$HERE/../src/ui/MainWindow.cpp"
HVCPP="$HERE/../src/ui/HomeView.cpp"
td_fail=0
td_note() { echo "  $1"; td_fail=1; }

# Line-comments stripped FIRST. Every one of these functions now carries a comment block that names
# NavMenu::pick / Osk::getText / PasscodePad::ask at length — explaining why they are NOT called there — so a gate
# that searched the raw text would fail on its own documentation.
if [ ! -f "$MWCPP" ] || [ ! -f "$HVCPP" ]; then
  echo "FAIL: themed handler deferral (MainWindow.cpp or HomeView.cpp not found under $HERE/../src/ui)"; fail=1
else
  mw_src="$(sed -E 's://.*$::' "$MWCPP")"
  hv_src="$(sed -E 's://.*$::' "$HVCPP")"
  # One file-scope function body, from its definition line to the column-0 `}` that closes it.
  td_body() { printf '%s\n' "$2" | awk -v sig="$1" '
    !on && index($0, sig) { on = 1 }
    on                    { print }
    on && /^\}/           { exit }
  '; }
  # The blocking nav-kit entry points. NavMenu's CALLBACK constructor (`new NavMenu(...)`) is asynchronous and
  # deliberately absent from this list — it is not a nested loop and never was the hazard.
  td_loops='NavMenu::pick|NavConfirm::ask|NavCountdown::ask|Osk::getText|PasscodePad::ask'
  # `grep -c`, never `grep -q`, when the haystack is a whole file. This script runs under `set -o pipefail`, and
  # `printf '%s' "$big" | grep -q needle` exits grep on the FIRST match — which SIGPIPEs printf as soon as the
  # text exceeds the pipe buffer, so the pipeline reports failure on a successful match. MainWindow.cpp is 13k
  # lines; the small extracted function bodies below fit in the buffer and never showed it. Counting reads to EOF.
  td_has() { [ "$(printf '%s\n' "$2" | grep -cF "$1")" != "0" ]; }

  # 1. deferPastQmlEmission must actually defer. A helper that quietly became a direct call would make every
  #    site below read as fixed while none of them were.
  td_defer="$(td_body 'void MainWindow::deferPastQmlEmission(' "$mw_src")"
  if [ -z "$td_defer" ]; then
    td_note "deferPastQmlEmission not found — signature changed? This gate is now asserting nothing about it."
  else
    printf '%s' "$td_defer" | grep -q 'Qt::QueuedConnection' \
      || td_note "deferPastQmlEmission does not use Qt::QueuedConnection: every caller believes it is getting a fresh event-loop turn, and none of them is."
  fi

  # 2. The themed detail action row. Its pill delegate emits the verb, and the verbs that open a nav-kit loop
  #    ("status", "tags", "editmeta", "playlist", "pcfix") must all be deferred — so no blocking call may appear
  #    on this function's own stack.
  td_rda="$(td_body 'void MainWindow::runThemedDetailAction(' "$mw_src")"
  if [ -z "$td_rda" ]; then
    td_note "runThemedDetailAction not found — signature changed? This gate is now asserting nothing about it."
  else
    td_hit="$(printf '%s' "$td_rda" | grep -nE "$td_loops" || true)"
    [ -n "$td_hit" ] && td_note "runThemedDetailAction spins a nested event loop on the QML emission's own stack: $(printf '%s' "$td_hit" | tr '\n' ' ')"
    td_n="$(printf '%s\n' "$td_rda" | grep -c 'deferPastQmlEmission' || true)"
    [ "$td_n" -ge 3 ] \
      || td_note "runThemedDetailAction has $td_n deferPastQmlEmission call(s); status, tags and editmeta each need one."
  fi

  # 3. The browse Filter menu. runThemedBrowseFilter is the deferring shim; the picks live in ...Now.
  td_bf="$(td_body 'void MainWindow::runThemedBrowseFilter()' "$mw_src")"
  if [ -z "$td_bf" ]; then
    td_note "runThemedBrowseFilter not found — signature changed? This gate is now asserting nothing about it."
  else
    printf '%s' "$td_bf" | grep -qE "$td_loops" \
      && td_note "runThemedBrowseFilter spins a nested event loop directly: it is the shim, and both of its callers are QML button emissions. The body belongs in runThemedBrowseFilterNow."
    printf '%s' "$td_bf" | grep -q 'deferPastQmlEmission' \
      || td_note "runThemedBrowseFilter does not defer — the Filter menu opens under the live delegate again."
  fi
  # ...and the deferred half must still be where the picks LIVE. Without this, a rename or an inline of
  # runThemedBrowseFilterNow leaves a shim deferring to nothing and the check above passing on an empty split —
  # the "this gate is now asserting nothing" failure mode, which is the one worth naming out loud.
  td_bfn="$(td_body 'void MainWindow::runThemedBrowseFilterNow()' "$mw_src")"
  if [ -z "$td_bfn" ]; then
    td_note "runThemedBrowseFilterNow not found — renamed or inlined? The shim above is then deferring to nothing and this gate is asserting nothing about where the Filter picks run."
  else
    td_bfn_loops="$(printf '%s\n' "$td_bfn" | grep -cE "$td_loops" || true)"
    [ "$td_bfn_loops" != "0" ] \
      || td_note "runThemedBrowseFilterNow contains no nav-kit loop: the Filter picks have moved somewhere this gate cannot see."
  fi

  # 4. The two detail pickers must take their key BY VALUE. They run a turn after the emission that asked for
  #    them, and the detail level's onPop clears themedDetailKey_ in between — a by-reference parameter would
  #    alias that member straight through the deferral AND the modal loops that follow it.
  for td_sig in 'void MainWindow::themedDetailPickStatus(QString key)' \
                'void MainWindow::themedDetailEditTags(QString key)'; do
    td_has "$td_sig" "$mw_src" \
      || td_note "expected '$td_sig' — a by-reference or no-argument form re-reads themedDetailKey_ across the deferral."
  done

  # 5. Every buildView cycleTheme / searchRequested handler. onCycle retires its OWN emitting widget
  #    (showThemed*() ends in stack_->removeWidget(old) + old->deleteLater()); onSearch spins Osk::getText and
  #    then re-sources a model or swaps the page. There are three of each — the grid home, the XMB and the
  #    browse view — and a new themed surface that forgets one is exactly how this pattern spread last time.
  for td_cb in onCycle onSearch; do
    td_count="$(printf '%s\n' "$mw_src" | grep -c "auto $td_cb = \[" || true)"
    if [ "$td_count" -lt 3 ]; then
      td_note "found $td_count 'auto $td_cb = [' handler(s), expected at least 3 — renamed or removed? This gate is now asserting less than it looks."
    fi
    # Each handler must reach deferPastQmlEmission within its own body (to the closing `    };`).
    td_bad="$(printf '%s\n' "$mw_src" | awk -v cb="auto $td_cb = [" '
      index($0, cb) { on = 1; seen = 0; start = NR }
      on && /deferPastQmlEmission/ { seen = 1 }
      on && /^    \};/ { if (!seen) print start; on = 0 }
    ')"
    [ -n "$td_bad" ] && td_note "$td_cb handler(s) at line(s) $(printf '%s' "$td_bad" | tr '\n' ' ')do not defer past the QML emission."
  done

  # 6. HomeView. addBrowseItemToPlaylist is where all three "add to a playlist" callers converge, and it is the
  #    one place that both resolves the row index and owns the deferral.
  td_abp="$(td_body 'void HomeView::addBrowseItemToPlaylist(' "$hv_src")"
  if [ -z "$td_abp" ]; then
    td_note "HomeView::addBrowseItemToPlaylist not found — signature changed? This gate is now asserting nothing about it."
  else
    printf '%s' "$td_abp" | grep -q 'Qt::QueuedConnection' \
      || td_note "addBrowseItemToPlaylist does not defer: its three callers are all live QML emissions and the picker it opens is a nested loop."
  fi

  # 7. The _newplaylist branch. createPlaylistInteractive runs Osk::getText and then rebuilds the level it is
  #    standing on; its two immediate siblings in the same if-chain were already queued and it was not.
  td_cpi="$(printf '%s\n' "$hv_src" | grep -n 'createPlaylistInteractive(' \
            | grep -v 'void HomeView::createPlaylistInteractive' || true)"
  if [ -z "$td_cpi" ]; then
    td_note "no call to createPlaylistInteractive found in HomeView.cpp — renamed? This gate is now asserting nothing about it."
  else
    td_undeferred="$(printf '%s\n' "$td_cpi" | grep -v 'invokeMethod' || true)"
    [ -n "$td_undeferred" ] && td_note "createPlaylistInteractive is called without a queued invoke: $(printf '%s' "$td_undeferred" | tr '\n' ' ')"
  fi

  # 8. THE THEME2 HOSTS (issue #165). MainWindow's handlers above are one half of the surface; the other half is
  #    the host classes that own their own QQuickWidget and dispatch a CALLER's std::function from a NavGraph
  #    signal. ThemedPanelHost::onGraphActivated and ThemePickerHost's two lambdas are DIRECT connections from
  #    NavGraph::activated / rootBack, and every production emitter of those is QML — SettingsPanel.qml's row
  #    delegate + header MouseAreas and root Keys handler, ThemePicker.qml's row delegate and root Keys handler.
  #    A caller handler dispatched from there runs on a live delegate's emission, and the shipped ones reach
  #    nested loops (the Emulators/Add-ons QFileDialogs, confirmRemoveAddon's NavConfirm, the startup Back's
  #    quit-confirm) — plus the panel host runs two loops of its OWN in the TextField editor. probe_navqml §18(k)
  #    pins the dispatch deferral as BEHAVIOUR for the panel host (it links headlessly); what is held here is the
  #    part no probe can drive: the TextField editor's own nested loops, and the picker host, which needs a real
  #    themes directory to present at all.
  TPHCPP="$HERE/../src/theme2/ThemedPanelHost.cpp"
  TPKCPP="$HERE/../src/theme2/ThemePickerHost.cpp"
  # Wider than td_loops: these two files also spin loops the nav kit does not own (the mobile inline edit's raw
  # QEventLoop, and the QFileDialog a caller opens).
  td_hostloops="$td_loops|QEventLoop|QFileDialog::"
  if [ ! -f "$TPHCPP" ] || [ ! -f "$TPKCPP" ]; then
    td_note "ThemedPanelHost.cpp or ThemePickerHost.cpp not found under $HERE/../src/theme2 — moved? This gate is now asserting nothing about the theme2 hosts."
  else
    tph_src="$(sed -E 's://.*$::' "$TPHCPP")"
    tpk_src="$(sed -E 's://.*$::' "$TPKCPP")"

    # 8a. Each host's own deferral helper must actually defer (the MainWindow check above, per host). Written out
    #     twice rather than looped: a `for` over "name:$src" pairs word-splits the embedded file on its newlines.
    td_hd="$(td_body 'void ThemedPanelHost::deferPastQmlEmission(' "$tph_src")"
    if [ -z "$td_hd" ]; then
      td_note "ThemedPanelHost::deferPastQmlEmission not found — every dispatch below believes it is getting a fresh event-loop turn. This gate is now asserting nothing about it."
    else
      printf '%s' "$td_hd" | grep -q 'Qt::QueuedConnection' \
        || td_note "ThemedPanelHost::deferPastQmlEmission does not use Qt::QueuedConnection: it is a direct call wearing the name of a deferral."
    fi
    td_hd="$(td_body 'void ThemePickerHost::deferPastQmlEmission(' "$tpk_src")"
    if [ -z "$td_hd" ]; then
      td_note "ThemePickerHost::deferPastQmlEmission not found — every dispatch below believes it is getting a fresh event-loop turn. This gate is now asserting nothing about it."
    else
      printf '%s' "$td_hd" | grep -q 'Qt::QueuedConnection' \
        || td_note "ThemePickerHost::deferPastQmlEmission does not use Qt::QueuedConnection: it is a direct call wearing the name of a deferral."
    fi

    # 8b. ThemedPanelHost::onGraphActivated — the panel host's dispatch boundary. FOUR row kinds dispatch from it
    #     (Action/Progress, Toggle, Choice, TextField) and every one must hand off through the helper; and no
    #     blocking loop may sit on its own body, because its own body IS the QML emission's stack.
    td_oga="$(td_body 'void ThemedPanelHost::onGraphActivated(' "$tph_src")"
    if [ -z "$td_oga" ]; then
      td_note "ThemedPanelHost::onGraphActivated not found — signature changed? This gate is now asserting nothing about the panel host's dispatch."
    else
      td_hit="$(printf '%s' "$td_oga" | grep -nE "$td_hostloops" || true)"
      [ -n "$td_hit" ] && td_note "ThemedPanelHost::onGraphActivated spins a nested event loop on the QML delegate's own stack: $(printf '%s' "$td_hit" | tr '\n' ' ')"
      td_n="$(printf '%s\n' "$td_oga" | grep -c 'deferPastQmlEmission' || true)"
      [ "$td_n" -ge 4 ] \
        || td_note "ThemedPanelHost::onGraphActivated has $td_n deferPastQmlEmission call(s); Action/Progress, Toggle, Choice and TextField each need one."
    fi
    # ...and the deferred TextField half must still be where the LOOPS live. Without this, inlining
    # runTextFieldEdit back into the switch leaves the count above satisfied by a deferral to nothing — the
    # "this gate is now asserting nothing" failure mode, same as runThemedBrowseFilterNow above.
    td_tfe="$(td_body 'void ThemedPanelHost::runTextFieldEdit(' "$tph_src")"
    if [ -z "$td_tfe" ]; then
      td_note "ThemedPanelHost::runTextFieldEdit not found — renamed or inlined? The TextField deferral is then deferring to nothing and this gate is asserting nothing about where the OSK runs."
    else
      td_tfe_loops="$(printf '%s\n' "$td_tfe" | grep -cE "$td_hostloops" || true)"
      [ "$td_tfe_loops" != "0" ] \
        || td_note "ThemedPanelHost::runTextFieldEdit contains no blocking editor: the OSK / inline-edit loop has moved somewhere this gate cannot see."
    fi

    # 8c. ThemedPanelHost::onLevelPopped — the ROOT onBack. It leaves the host (a QQuickWidget retirement) or
    #     opens a quit-confirm, from a nav.back() emission, so it defers; the in-host sub-panel pop above it must
    #     NOT (probe_navqml §18(k)(v) holds that converse). A bare `gone.onBack()` is the pre-#165 shape.
    td_olp="$(td_body 'void ThemedPanelHost::onLevelPopped(' "$tph_src")"
    if [ -z "$td_olp" ]; then
      td_note "ThemedPanelHost::onLevelPopped not found — signature changed? This gate is now asserting nothing about the root onBack."
    else
      printf '%s' "$td_olp" | grep -q 'deferPastQmlEmission' \
        || td_note "ThemedPanelHost::onLevelPopped does not defer: the root onBack tears down this host's QQuickWidget from inside its own scene's emission again."
      td_bare="$(printf '%s' "$td_olp" | grep -nE '(^|[^.[:alnum:]_])gone\.onBack\(\)' || true)"
      [ -n "$td_bare" ] && td_note "ThemedPanelHost::onLevelPopped calls gone.onBack() directly: $(printf '%s' "$td_bare" | tr '\n' ' ')"
    fi

    # 8d. ThemePickerHost — both caller dispatches (the row pick, and rootBack, whose startup form is the
    #     NavConfirm quit prompt) live in the constructor's connect lambdas. They must go through the helper, and
    #     the bare `fn(folder);` / `fn();` shape they replaced must not come back.
    td_tpk="$(td_body 'ThemePickerHost::ThemePickerHost(' "$tpk_src")"
    if [ -z "$td_tpk" ]; then
      td_note "ThemePickerHost's constructor not found — signature changed? This gate is now asserting nothing about the picker's dispatch."
    else
      td_n="$(printf '%s\n' "$td_tpk" | grep -c 'deferPastQmlEmission' || true)"
      [ "$td_n" -ge 2 ] \
        || td_note "ThemePickerHost's constructor has $td_n deferPastQmlEmission call(s); the pick dispatch and the rootBack dispatch each need one."
      td_bare="$(printf '%s' "$td_tpk" | grep -nE '^[[:space:]]*fn\((folder)?\);' || true)"
      [ -n "$td_bare" ] && td_note "ThemePickerHost dispatches a caller callback directly on the QML emission's stack: $(printf '%s' "$td_bare" | tr '\n' ' ')"
    fi
  fi

  if [ "$td_fail" -eq 0 ]; then echo "PASS: themed handler deferral"; else
    echo "FAIL: themed handler deferral (a themed handler runs a nested event loop on a live QML delegate's stack)"; fail=1
  fi
fi
echo

# Panel-dialog lifetime gate (issue #122). Same standing as the gate above, and for the same reason: MainWindow
# links into no probe, so this rule cannot be asserted as behaviour anywhere. It is held as source shape.
#
# THE DEFECT, from the preserved dump. showPanel replaces the panel's content with
# `panelScroll_->setWidget(content)`, which deletes the PREVIOUS content widget synchronously. A dialog put
# there by showDialogPanel is a CHILD of that content, so the call destroys it — while panelDialog_ still names
# it. The next statement, `stack_->setCurrentWidget(panelPage_)`, emits QStackedWidget::currentChanged, whose
# slot is updateNavForPage(), which runs `panelDialog_ && panelDialog_->inherits("ControllerRemapDialog")`: a
# virtual dispatch through a dead object. In the dump that is Qt6Core!QObject::inherits+0x7 reading [rax+8]
# with rax = 1 (the freed block's first qword, where the vptr used to be), one frame under
# MainWindow::updateNavForPage. The classic-mode path that reaches it is ordinary: the startup profile picker
# is a showDialogPanel, openHome() leaves the panel page without clearing anything, and the first Settings
# panel after that is the showPanel that frees the picker out from under the pointer.
#
# THE RULE, in two independent halves:
#   a. panelDialog_ is a QPointer, so it nulls itself when the dialog dies, by any route.
#   b. showPanel clears it BEFORE the setWidget that does the destroying, so the window never opens.
# Either alone closes #122. Both are held, because (a) covers destruction routes (b) cannot see, and (b) keeps
# the order right for a reader who has not noticed (a).
#
# What this gate CANNOT see: whether some future code path stores a third alias to the hosted dialog and
# outlives it that way. That stays a review obligation, written on panelDialog_'s declaration.
echo "=== panel dialog lifetime ==="
PDL_CPP="$HERE/../src/ui/MainWindow.cpp"
PDL_H="$HERE/../src/ui/MainWindow.h"
pdl_fail=0
pdl_note() { echo "  $1"; pdl_fail=1; }
if [ ! -f "$PDL_CPP" ] || [ ! -f "$PDL_H" ]; then
  echo "FAIL: panel dialog lifetime (MainWindow.cpp or MainWindow.h not found under $HERE/../src/ui)"; fail=1
else
  # Comments stripped first and CRs dropped: the declaration and showPanel both carry comment blocks that quote
  # the very tokens matched below (this repo is CRLF, so a `$`-anchored pattern on a raw line matches nothing).
  pdl_hsrc="$(sed -E 's://.*$::' "$PDL_H" | tr -d '\r')"
  pdl_csrc="$(sed -E 's://.*$::' "$PDL_CPP" | tr -d '\r')"

  # a. The declaration. A raw QWidget* here is freed-but-non-null for as long as it takes the currentChanged
  #    slot to type-test it.
  # COUNTS, not `grep -q`: MainWindow.h is ~100 KB and this suite runs under `set -o pipefail`, so grep -q
  # exiting on the first match SIGPIPEs the printf that feeds it — a real match then fails the pipeline. grep -c
  # reads to EOF, so the pipe never closes early. See the note above the metadata-editor gate.
  [ "$(printf '%s\n' "$pdl_hsrc" | grep -cE '^[[:space:]]*QPointer<[[:space:]]*QWidget[[:space:]]*>[[:space:]]+panelDialog_' || true)" != "0" ] \
    || pdl_note "panelDialog_ is not declared 'QPointer<QWidget> panelDialog_' in MainWindow.h: a raw pointer to a panel-hosted dialog outlives the dialog, and updateNavForPage() dereferences it (#122)."

  # b. The ordering inside showPanel. Body = the definition line through the column-0 brace that closes it.
  pdl_body="$(printf '%s\n' "$pdl_csrc" | awk '
    !on && index($0, "void MainWindow::showPanel(") { on = 1 }
    on           { print }
    on && /^\}/  { exit }')"
  if [ -z "$pdl_body" ]; then
    pdl_note "MainWindow::showPanel not found — signature changed? This gate is now asserting nothing about the clear/destroy order."
  else
    # Line numbers WITHIN the extracted body, so an edit elsewhere in the file cannot move them.
    pdl_clear="$(printf '%s\n' "$pdl_body" | grep -n 'panelDialog_[[:space:]]*=[[:space:]]*nullptr' | head -1 | cut -d: -f1)"
    pdl_set="$(printf '%s\n' "$pdl_body" | grep -n 'panelScroll_->setWidget(' | head -1 | cut -d: -f1)"
    if [ -z "$pdl_clear" ]; then
      pdl_note "showPanel no longer clears panelDialog_: a plain panel would inherit the previous panel's dialog for its nav ring, and the pointer would outlive the object it names."
    elif [ -z "$pdl_set" ]; then
      pdl_note "showPanel no longer calls panelScroll_->setWidget( — the content-replacement point this gate orders against has moved, and the ordering half is asserting nothing."
    elif [ "$pdl_clear" -gt "$pdl_set" ]; then
      pdl_note "showPanel clears panelDialog_ (body line $pdl_clear) AFTER panelScroll_->setWidget (body line $pdl_set): setWidget destroys the hosted dialog, so the pointer dangles across the setCurrentWidget that follows — and that emits currentChanged into updateNavForPage(). That is #122."
    fi
  fi

  if [ "$pdl_fail" -eq 0 ]; then echo "PASS: panel dialog lifetime"; else
    echo "FAIL: panel dialog lifetime (a panel-hosted dialog can be type-tested after it is destroyed)"; fail=1
  fi
fi
echo

# uitest channel startup ORDER gate (issue #177). probe_uitest §9 pins the predicate — wanted(NotSettled) does
# not read Settings — but a probe links UiTestServer, not main(), so it cannot see WHICH phase main() declares.
# Saying Settled at the early call site compiles, passes every probe, and reinstates the original bug in full:
# Settings::store() snapshots the ini on its first read, so one read before BrandMigration has copied the file
# into place runs the rest of the session off a pre-migration (usually absent) settings file. The only symptom
# is that an EB_UITEST launch behaves as though the user's settings were empty — which reads as "the test
# environment is odd", not as a data bug. So the two call sites and the migration between them are ordered here.
echo "=== uitest channel startup order ==="
MAINCPP="$HERE/../src/main.cpp"
uo_fail=0
uo_note() { echo "  $1"; uo_fail=1; }
if [ ! -f "$MAINCPP" ]; then
  uo_note "main.cpp not found at $MAINCPP"
else
  # Comments stripped, line numbers preserved (sed emits one line per line). The prose around both call sites
  # quotes the old spelling, so an un-stripped grep would match the history instead of the code.
  uo_src="$(sed -E 's://.*$::' "$MAINCPP")"
  # COUNTS, not `grep -q`: this suite runs under `set -o pipefail` and -q exits on the first match, SIGPIPEing
  # the printf that feeds it — a match then fails the pipeline. See the note above the metadata-editor gate.
  uo_line() { printf '%s\n' "$uo_src" | grep -n "$1" | head -1 | cut -d: -f1; }
  uo_count() { printf '%s\n' "$uo_src" | grep -c "$1" || true; }

  uo_early="$(uo_line 'UiTestServer::ensureListening(UiTestServer::IniPhase::NotSettled)')"
  uo_late="$(uo_line 'UiTestServer::ensureListening(UiTestServer::IniPhase::Settled)')"
  uo_mig="$(uo_line '^[[:space:]]*brandMigrationAtStartup();')"

  if [ "$(uo_count 'UiTestServer::ensureListening(UiTestServer::IniPhase::NotSettled)')" != "1" ]; then
    uo_note "main.cpp does not have exactly one ensureListening(IniPhase::NotSettled) call. The pre-migration channel is what issue #172 added and what #177 pinned; if it moved, move this gate with it deliberately."
  elif [ "$(uo_count 'UiTestServer::ensureListening(UiTestServer::IniPhase::Settled)')" != "1" ]; then
    uo_note "main.cpp does not have exactly one ensureListening(IniPhase::Settled) call — the Settings-toggle half of the channel is gone, or duplicated."
  elif [ -z "$uo_mig" ]; then
    uo_note "main.cpp no longer calls brandMigrationAtStartup(); — the point this gate orders against has moved and the ordering is asserting nothing."
  elif [ "$uo_early" -ge "$uo_mig" ]; then
    uo_note "the NotSettled ensureListening (line $uo_early) is not BEFORE brandMigrationAtStartup() (line $uo_mig). Its whole purpose is to have the channel up while the settings-dependent startup work runs; after the migration it is just a slower copy of the Settled call, and a stall before this point leaves no pipe again (#172)."
  elif [ "$uo_late" -le "$uo_mig" ]; then
    uo_note "the Settled ensureListening (line $uo_late) runs BEFORE brandMigrationAtStartup() (line $uo_mig). Settings::store() snapshots the ini on its first read, so this reads the Debug toggle out of a pre-migration file and pins the whole session to it (#177)."
  fi
fi
if [ "$uo_fail" -eq 0 ]; then echo "PASS: uitest channel startup order"; else
  echo "FAIL: uitest channel startup order (a settings read can now happen before the brand migration settles the ini)"; fail=1
fi
echo

# Post-merge add-on-ref repair gate (#58 review). CloudMerge's tie-break no longer lets an equal-timestamp
# meeting be decided on an add-on id's SPELLING, so a repaired favourite/playlist is no longer reverted by the
# merge that follows it — probe_cloudmerge section 19 proves that end to end. But a peer's blob that genuinely
# IS newer still wins outright, spelling and all, and it can land this device back on the namespace it renamed
# away from. AddonManager::reload() has already run for the session by then, so the repair has to run again
# AFTER the merge or the favourite reads "source addon isn't available" until the next launch.
#
# That second half is a WIRING fact inside MainWindow, which no headless probe links (it is the whole Qt
# Widgets app). The probe demonstrates the behaviour by calling merge-then-reconcile itself; this gate pins
# that mergeProgress is where the product actually does it. Deleting the call is otherwise a silent revert:
# every probe stays green, and the symptom only appears on a synced install with a renamed add-on.
echo "=== post-merge addon-ref repair ==="
MWCPP="$HERE/../src/ui/MainWindow.cpp"
if [ ! -f "$MWCPP" ]; then
  echo "FAIL: post-merge addon-ref repair (MainWindow.cpp not found at $MWCPP)"; fail=1
else
  # The body of mergeProgress, comments stripped, from its signature to the closing brace at column 0.
  mp_body="$(sed -E 's://.*$::' "$MWCPP" | awk '
    /^void MainWindow::mergeProgress\(/ { inbody = 1 }
    inbody                              { print }
    inbody && /^}/                      { exit }
  ')"
  if [ -z "$(printf '%s' "$mp_body" | tr -d '[:space:]')" ]; then
    echo "FAIL: post-merge addon-ref repair (MainWindow::mergeProgress not found — the gate stopped matching)"
    fail=1
  elif ! printf '%s' "$mp_body" | grep -q 'reconcileAddonRefs'; then
    echo "FAIL: post-merge addon-ref repair (mergeProgress no longer re-runs BrandMigration::reconcileAddonRefs)"
    fail=1
  else
    echo "PASS: post-merge addon-ref repair"
  fi
fi
echo

# Metadata-editor baseline gate (issue #24 review). The editor corrects an item against what the PROVIDERS
# said, and the richest copy of that is the live /meta reply the open card was drawn from — richer than the
# scrape cache — so HomeView holds it. It is written only when a reply ARRIVES: an item whose addon returns
# nothing (offline, or gone upstream) writes none, and an UNKEYED member is then still holding the PREVIOUS
# item's card. The editor, opened on this item's key, seeded its OSK from another item's title/synopsis/
# poster, ran the "typed back what the scraper found -> store nothing" comparison against them, and wrote
# them into THIS item's override — which CloudMerge carries to every device. A corrupted record, not a pixel.
#
# MetaEdit::ScrapedSnapshot makes that unspellable, and probe_meta asserts the type itself (remember(A), then
# forKey(B) is invalid). HomeView is the Qt Widgets app, which no headless probe links, so what is gated here
# is the WIRING: the member IS that type, the read goes through forKey(), and nothing in HomeView touches it
# any other way. Going back to `if (scrapedDetail_.valid) return scrapedDetail_;` leaves every probe green
# and resurfaces only as one item's synopsis stored under another item's key, on every device.
echo "=== metadata-editor baseline (keyed scrape snapshot) ==="
HVCPP="$HERE/../src/ui/HomeView.cpp"
HVH="$HERE/../src/ui/HomeView.h"
ms_fail=0
ms_note() { echo "  $1"; ms_fail=1; }
# COUNTS, never `grep -q`, for anything fed from a variable: the suite runs under `set -o pipefail`, and -q
# exits at the first match — which SIGPIPEs the printf still writing a 5,000-line source blob into it, so the
# PIPELINE reports failure and the gate announces a violation that isn't there. A gate that cries wolf is one
# people learn to skip, which is worse than not having it. (`grep -q` on a real FILE is fine; nothing is
# writing into it. The existing gates above feed grep single function bodies, small enough to never block.)
ms_n() { printf '%s\n' "$2" | grep -c "$1" || true; }
if [ ! -f "$HVCPP" ] || [ ! -f "$HVH" ]; then
  ms_note "HomeView.{h,cpp} not found under $HERE/../src/ui — this gate is asserting nothing."
else
  grep -qE 'MetaEdit::ScrapedSnapshot[[:space:]]+scrapedDetail_' "$HVH" \
    || ms_note "HomeView.h no longer declares scrapedDetail_ as a MetaEdit::ScrapedSnapshot — a bare MediaDetail can be read without naming the item it belongs to, which is the whole defect."
  # Line comments stripped, so the prose above the member (which discusses it at length) never trips this.
  ms_src="$(sed -E 's://.*$::' "$HVCPP")"
  # Every mention of the member with the two LEGAL spellings deleted; whatever still matches is a raw read
  # or a raw assignment.
  ms_raw="$(printf '%s\n' "$ms_src" | grep -n 'scrapedDetail_' \
            | sed -E 's/scrapedDetail_\.(forKey|remember)\(//g' | grep 'scrapedDetail_' || true)"
  if [ -n "$ms_raw" ]; then
    ms_note "scrapedDetail_ is touched outside forKey()/remember() — an unkeyed read is the defect itself:"
    printf '%s\n' "$ms_raw" | sed 's|^|    |'
  fi
  [ "$(ms_n 'scrapedDetail_\.remember(' "$ms_src")" -ge 1 ] \
    || ms_note "nothing stamps the snapshot any more (showMeta's fromProvider branch): the editor drops back to the cache for every item, and the open card visibly strips on each edit."
  ms_body="$(printf '%s\n' "$ms_src" | awk '
    /^MediaDetail HomeView::detailScrapedValues\(\) const/ { inbody = 1 }
    inbody { print }
    inbody && /^}/ { exit }')"
  if [ -z "$(printf '%s' "$ms_body" | tr -d '[:space:]')" ]; then
    ms_note "HomeView::detailScrapedValues not found — the gate stopped matching its signature."
  else
    [ "$(ms_n 'scrapedDetail_\.forKey(' "$ms_body")" -ge 1 ] \
      || ms_note "detailScrapedValues does not read the snapshot through forKey()."
    [ "$(ms_n 'MetaCache::keyFor(' "$ms_body")" -ge 1 ] \
      || ms_note "detailScrapedValues no longer derives the open item's key — forKey() is only as honest as the key handed to it."
  fi

  # The themed half of the same feature. The live panel's map is assembled from five SCRAPED sources — the
  # catalog row, the ROMs-folder gamelist.xml, this session's art cache, our scrape cache, and the addon's
  # /meta — and none of them knows about the correction. Emitting any of them raw put the scraped synopsis
  # and poster back over the correction a moment after the detail page opened, so the feature worked only
  # with the network down (the offline branch goes through cachedDetail, which composites). One emitter,
  # compositing the FINISHED map, is what makes that unrepeatable — and what lets the session art cache go
  # on holding the scraped map, so an edit or a reset needs no cache invalidation to show.
  ms_emits="$(ms_n 'emit themedMetaReady(' "$ms_src")"
  ms_fn="$(printf '%s\n' "$ms_src" | awk '
    /^void HomeView::emitThemedMeta\(/ { inbody = 1 }
    inbody { print }
    inbody && /^}/ { exit }')"
  if [ -z "$(printf '%s' "$ms_fn" | tr -d '[:space:]')" ]; then
    ms_note "HomeView::emitThemedMeta not found — the single-emitter funnel is gone, so every themed source emits its own raw map again."
  else
    [ "$(ms_n 'MetaOverrides::applyTo(' "$ms_fn")" -ge 1 ] \
      || ms_note "emitThemedMeta no longer composites the correction over the map it emits — the panel and the detail card show the scrape, and the session art cache pins it there."
    [ "$(ms_n 'emit themedMetaReady(' "$ms_fn")" -ge 1 ] \
      || ms_note "emitThemedMeta does not emit themedMetaReady — the funnel stopped being the funnel."
  fi
  [ "$ms_emits" = "1" ] \
    || ms_note "expected exactly ONE 'emit themedMetaReady(' in HomeView.cpp (emitThemedMeta's); found $ms_emits — a raw emit bypasses the composite."

  # The items_ ingress. Every surface that reads items_ — the poster grid, the carousel, the XMB column, the
  # themed browse model, search — gets the correction from ONE composite on the way in. The first fix did
  # only populate(), and renderRecents builds its rows separately: the recents groups, the Favorites section
  # and the Trakt "Airing Soon" shelf each pushed straight into items_ and into their QListWidgetItem labels.
  # The poster on those same rows WAS corrected (it goes through MetaCache), so Home — the screen the app
  # lands on — showed fixed art beside an unfixed title. Three shelves, so three call sites.
  ms_fnbody() { printf '%s\n' "$ms_src" | awk -v sig="$1" '
    index($0, sig) == 1 { inbody = 1 }
    inbody              { print }
    inbody && /^\}/     { exit }'; }
  ms_cr="$(ms_fnbody 'MediaItem HomeView::correctedRow(')"
  if [ -z "$(printf '%s' "$ms_cr" | tr -d '[:space:]')" ]; then
    ms_note "HomeView::correctedRow not found — the single items_ ingress is gone."
  else
    [ "$(ms_n 'MetaOverrides::applyTo(' "$ms_cr")" -ge 1 ] \
      || ms_note "correctedRow no longer composites the correction — every items_ surface goes back to the scrape."
    [ "$(ms_n 'preCorrection_' "$ms_cr")" -ge 1 ] \
      || ms_note "correctedRow no longer keeps the pre-correction row: the composite is destructive, so the metadata editor loses the scraped baseline it compares and resets against."
  fi
  ms_rec="$(ms_n 'correctedRow(' "$(ms_fnbody 'void HomeView::renderRecents()')")"
  [ "${ms_rec:-0}" -ge 3 ] \
    || ms_note "renderRecents composites the correction at $ms_rec of its 3 row sources (recents groups, Favorites, the Trakt shelf) — Home shows corrected art beside an uncorrected title for the ones it misses."
  ms_pop="$(ms_n 'correctedRow(' "$(ms_fnbody 'void HomeView::populate(')")"
  [ "${ms_pop:-0}" -ge 1 ] \
    || ms_note "populate() no longer composites the correction into the catalog rows."
fi
if [ "$ms_fail" -eq 0 ]; then echo "PASS: metadata-editor baseline (keyed scrape snapshot)"; else
  echo "FAIL: metadata-editor baseline (keyed scrape snapshot)"; fail=1
fi
echo

# Proxy-header log discipline (#43). A stream's behaviorHints.proxyHeaders routinely carry a signed-URL
# token, a session cookie or an Authorization value, and stream_debug.log is a file users paste into bug
# reports. probe_stremio pins that StreamHeaders::logSummary emits NAMES and never values; this gate pins the
# other half — that logSummary is the ONLY way header data reaches a log. Without it the rule holds exactly
# until someone writes the obvious .arg(headers.value("Referer")) into a trace line, which no probe can see.
#
# The first version of this gate caught the mutation its author tried and very little else. Every mechanism
# below exists because a realistic spelling walked straight past it:
#
#   * The corpus was `src/**/*.cpp` — a glob that REQUIRES a directory component, so top-level src/*.cpp was
#     excluded. That is 1 file out of 113, and it is main.cpp: the file that owns appLogHandler and logPath(),
#     i.e. the log itself. Both patterns are listed now, and ph_scanned/ph_toplevel below make a corpus that
#     silently stops matching a FAILURE rather than a pass.
#   * Comments were stripped with `s://.*$::`, which also eats `http://` inside a string literal and truncates
#     the line before the greps ever see it. String literals are blanked FIRST now, so a `//` that survives is
#     genuinely a comment.
#   * Matching was line-at-a-time, so a wrapped call was invisible. The source is folded into STATEMENTS first.
#   * `grep -v logSummary` whitelisted the whole line, so `srLog(logSummary(h) + raw)` passed. logSummary(...)
#     sub-expressions are now DELETED from the statement and whatever remains is matched.
#   * The helper list named 6 of them. It is a shape now (`…Log(`), so pfLog/glLog/ieLog/videoLog/loadLog and
#     the next one someone writes are covered, plus qCritical/qFatal/qC*.
#   * A value assigned to a temp first (`const QString r = headers.value("Referer"); streamLog(r);`) is not
#     something a grep for log calls can see at all. It is cut off at the source instead: reading a header
#     VALUE is confined to StreamHeaders.cpp, so outside it there is no temp to log.
echo "=== proxy-header log discipline ==="
ph_fail=0
ph_note() { echo "  $1"; ph_fail=1; }

# The corpus. BOTH spellings: 'src/*.cpp' matches only top-level, 'src/**/*.cpp' matches only nested.
ph_files="$(git -C "$HERE/.." ls-files 'src/*.cpp' 'src/*.h' 'src/**/*.cpp' 'src/**/*.h' 2>/dev/null)"
ph_scanned="$(printf '%s\n' "$ph_files" | grep -c '[^[:space:]]' || true)"
ph_toplevel="$(printf '%s\n' "$ph_files" | grep -cE '^src/[^/]+\.(cpp|h)$' || true)"
# "Did I scan anything?" — the guard the neighbouring crashreport gate has and this one did not. A gate that
# scans an empty corpus prints PASS, which is worse than having no gate: it reports a rule as enforced.
# The floor is deliberately far below the real count (113) and far above zero: it catches a glob that broke,
# a `git ls-files` run from the wrong directory, and a repo layout that moved out from under this script.
if [ "$ph_scanned" -lt 50 ]; then
  ph_note "corpus is $ph_scanned file(s) — expected the whole of src/. This gate scanned almost nothing; treat its PASS as meaningless until the file list is fixed."
fi
# …and specifically that top-level src/*.cpp is in it, which is the exact hole the first version had.
if [ "$ph_toplevel" -lt 1 ]; then
  ph_note "no top-level src/*.cpp in the corpus — the '**' glob excludes them, and that is where main.cpp (appLogHandler, logPath) lives."
fi

# The whole corpus, normalised into "path:line: statement" records, in ONE awk pass. Per-file sed|awk
# pipelines cost a process pair each; at 240-odd files that was ~90s of the suite's wall clock, and a gate
# slow enough to be annoying is a gate someone eventually skips. Each line is normalised before folding:
#   1. blank the contents of string literals, so a `//` inside "http://x" cannot be mistaken for a comment
#      and a log message's own prose cannot match a pattern;
#   2. strip what is then genuinely a line comment;
#   3. fold continuation lines together until the statement ends, so a wrapped call is ONE record.
ph_blob="$(cd "$HERE/.." && awk '
    FNR == 1 && buf != "" { print prevf ":" start ": " buf; buf = "" }
    { s = $0
      gsub(/"[^"]*"/, "\"\"", s)
      sub(/\/\/.*$/, "", s)
      sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s)
      if (buf == "") { start = FNR; buf = s; prevf = FILENAME } else if (s != "") { buf = buf " " s }
      if (s ~ /[;{}]$/ || s == "") { if (buf != "") print FILENAME ":" start ": " buf; buf = ""; start = 0 } }
    END { if (buf != "") print prevf ":" start ": " buf }' $ph_files </dev/null)"
# (</dev/null is not decoration: with an empty corpus awk gets no file operands and falls back to STDIN,
# which under CI is the suite's own stdin — the gate would hang rather than report the empty corpus above.)

# A log call, in any of the tree's spellings, whose statement still mentions header data after every
# logSummary(...) sub-expression has been removed from it. The `:a;ta` loop peels nested parens.
ph_hits="$(printf '%s\n' "$ph_blob" \
     | grep -E '[A-Za-z_][A-Za-z0-9_]*Log[[:space:]]*\(|q(Debug|Warning|Info|Critical|Fatal|C[A-Za-z]+)[[:space:]]*\(' \
     | sed -E ':a; s/(StreamHeaders::)?logSummary\([^()]*\)//g; ta' \
     | grep -E 'requestHeaders|proxyHeaders|StreamHeaders::Headers|[A-Za-z_]*[Hh]eaders?[A-Za-z0-9_]*[[:space:]]*(\.[[:space:]]*value[[:space:]]*\(|\[)' || true)"
if [ -n "$ph_hits" ]; then
  ph_note "a log call touches header data without going through StreamHeaders::logSummary:"
  printf '%s\n' "$ph_hits" | sed 's|^|    |'
fi

# Reading a header VALUE lives in StreamHeaders.cpp and nowhere else. This is the rule that closes the
# temp-variable hole: no grep over log calls can see `const QString r = headers.value("Referer");` two lines
# earlier, so the value is never allowed to become a local outside the one file whose job it is.
ph_reads="$(printf '%s\n' "$ph_blob" | grep -v '^src/core/StreamHeaders\.cpp:' \
     | grep -E '[A-Za-z_]*[Hh]eaders?[A-Za-z0-9_]*[[:space:]]*(\.[[:space:]]*value[[:space:]]*\(|\[)' || true)"
if [ -n "$ph_reads" ]; then
  ph_note "a header VALUE is read outside StreamHeaders.cpp — pass the whole container instead, or the value becomes a local nothing can trace to a log:"
  printf '%s\n' "$ph_reads" | sed 's|^|    |'
fi

# …and the naming rule that makes the one above actually hold. The check for a value read hooks on the
# CONTAINER'S NAME, so `const StreamHeaders::Headers& hdrs; … hdrs.value("Referer")` would sail past it. A
# StreamHeaders::Headers therefore has to be named for what it is. Two cheap rules composing into a real
# guarantee beats one clever rule that only looks like one.
# ($1 is the "path:line:" prefix every blob record starts with. The qualified-return-type spelling
# `StreamHeaders::Headers StreamHeaders::parseProxyHeaders` binds the name "StreamHeaders", which contains
# "Headers" and so exempts itself — correctly, since it is not a variable at all.)
ph_names="$(printf '%s\n' "$ph_blob" | awk '
  { rest = $0
    while (match(rest, /StreamHeaders::Headers[ \t]*[&*]?[ \t]*[A-Za-z_][A-Za-z0-9_]*/)) {
      m = substr(rest, RSTART, RLENGTH); rest = substr(rest, RSTART + RLENGTH)
      name = m; sub(/^StreamHeaders::Headers[ \t]*[&*]?[ \t]*/, "", name)
      if (name !~ /[Hh]eaders?[A-Za-z0-9_]*$/) print $1 " " m
    } }')"
if [ -n "$ph_names" ]; then
  ph_note "a StreamHeaders::Headers is bound to a name with no 'header' in it — the value-read check above matches on the container's name, so this one is invisible to it. Rename it:"
  printf '%s\n' "$ph_names" | sed 's|^|    |'
fi

# logSummary itself must keep emitting keys, not values. Cheap, but it is the assertion the gate rests on,
# and a probe cannot notice the day the file stops existing.
if ! grep -q 'h.keys()' "$HERE/../src/core/StreamHeaders.cpp" 2>/dev/null; then
  ph_note "StreamHeaders::logSummary no longer renders h.keys() — check it is still names-only."
fi
if grep -qE 'logSummary' "$HERE/../src/core/StreamHeaders.cpp" 2>/dev/null; then :; else
  ph_note "StreamHeaders::logSummary not found — this gate is now asserting nothing."
fi
if [ "$ph_fail" -eq 0 ]; then echo "PASS: proxy-header log discipline ($ph_scanned files scanned)"; else
  echo "FAIL: proxy-header log discipline"; fail=1
fi
echo

# Old-brand gate (rebrand T4): the product was renamed, and "no mentions of the previous name remain" has to be
# a property the suite ENFORCES rather than a claim someone made once — otherwise the next person to type
# "MyMediaVault" into a comment reintroduces it and nothing notices. Everything that still names the old brand
# is listed below WITH ITS REASON, because an unexplained exemption is indistinguishable from an oversight.
# Anything not on this list is a leftover — or a new one someone just typed — and it fails here.
#
#   * run-headless-probes.sh          — this gate; it has to contain the pattern it searches for.
#   * AppBrand.h                      — the Legacy:: block IS the previous identity, by definition.
#   * BrandMigration{,Drive}.cpp      — the migration that moves installs off the old brand; it is its subject.
#   * main.cpp                        — NO LONGER EXEMPT (#121). Its one exempt region was the
#                                       Goliath->MyMediaVault hop, carved out by an awk range because the old
#                                       name is that function's subject matter. The function now lives in
#                                       BrandMigration.cpp (already exempt, and where probe_brand can reach
#                                       it), so main.cpp is gated in full like every other file — which is
#                                       strictly stronger than the range was, since the range trusted a
#                                       comment's first line to stay put.
#   * aiocatalog-worker/{worker.js,wrangler.toml,README.md}
#                                     — USER DECISION: the deployed Worker's name (hence its workers.dev
#                                       hostname), the add-on id com.mymediavault.aiocatalog-worker, and the
#                                       X-MMV-Config header the old revision reads. Renaming the id or the
#                                       hostname orphans every add-on URL a user has already saved. Left
#                                       deliberately; its README documents them, so it is exempt too.
#   * probe_playlists.cpp             — the v1 legacy playlist blob is a FIXTURE of what old installs wrote.
#                                       Renaming the strings inside it destroys what the test tests.
#   * native/secrets/README.md        — documents the EXISTING Android release keystore: alias `mmv-release`
#                                       and CN=MyMediaVault are baked into that keystore file. Renaming the
#                                       prose would make it false and the ANDROID_KEY_ALIAS secret wrong.
#   * native/resources/Uninstall.cmd  — must still delete the PRE-rename state. `%LOCALAPPDATA%\My Media Vault`
#                                       is the live cache dir (setApplicationName is still the legacy spaced
#                                       form — see main.cpp:214), and this uninstaller exists precisely for the
#                                       case where the app never launched, i.e. where migration never ran.
#   * the rebrand's own spec + plan   — a document explaining a rename has to name what was renamed.
echo "=== old-brand references ==="
BRAND_PATTERNS=(-e 'mymediavault' -e 'my media vault' -e '\bMMV\b' -e 'MMV_')
brand_hits="$(cd "$HERE/../.." && git grep -I -n -i "${BRAND_PATTERNS[@]}" \
  -- . ':(exclude)native/tools/run-headless-probes.sh' \
       ':(exclude)native/src/core/AppBrand.h' \
       ':(exclude)native/src/core/BrandMigration.cpp' \
       ':(exclude)native/src/core/BrandMigrationDrive.cpp' \
       ':(exclude)native/addon-protocol/aiocatalog-worker/src/worker.js' \
       ':(exclude)native/addon-protocol/aiocatalog-worker/wrangler.toml' \
       ':(exclude)native/addon-protocol/aiocatalog-worker/README.md' \
       ':(exclude)native/tools/probe_playlists.cpp' \
       ':(exclude)native/secrets/README.md' \
       ':(exclude)native/resources/Uninstall.cmd' \
       ':(exclude)docs/superpowers/specs/2026-07-27-everythingbox-rebrand-design.md' \
       ':(exclude)docs/superpowers/plans/2026-07-27-everythingbox-rebrand-plan.md' || true)"
# main.cpp needs no carve-out any more (#121): the Goliath hop moved to BrandMigration.cpp, so the file is
# covered by the git grep above like everything else. Its continued existence is still asserted, because the
# gate silently passing on a file that vanished is the failure mode this whole block exists to avoid.
MAINCPP="$HERE/../src/main.cpp"
[ -f "$MAINCPP" ] || { echo "FAIL: old-brand references (main.cpp not found at $MAINCPP)"; fail=1; }
if [ -n "$(printf '%s' "$brand_hits" | tr -d '[:space:]')" ]; then
  echo "$brand_hits"
  echo "FAIL: old-brand references (the previous name survives outside the documented exemptions above)"
  fail=1
else
  echo "PASS: old-brand references"
fi
echo

# Release-asset name gate (issue #35): the README's download table links straight at
# releases/latest/download/<asset>, so a filename that no release job actually produces is a 404 on the
# repo's front page — for every visitor, on every platform, until someone notices. That is exactly what the
# rebrand did: the README moved to EverythingBox-* while the assets published for v0.5.0 were still
# MyMediaVault-*. The old-brand gate above could not catch it, because the stale names live on GitHub
# Releases, not in the tree. This gate catches the in-tree half of the same class: a README download link
# whose filename no `softprops/action-gh-release` step in release.yml attaches. It cannot see what is
# already published — renaming assets on a cut release is a manual Releases operation.
#
# Source of truth is the `files:` list of each release-attach step (inline or `|` block), with the Android
# job's ${{ matrix.name }} expanded over the ABIs its matrix fans out over. Direction is one-way on purpose:
# every README link must be produced, but the workflow may publish extras the table doesn't list (the
# armv7/x86_64 APKs and the -pdb.zip symbol archive are deliberately unlisted).
echo "=== release asset names (README <-> release.yml) ==="
RELYML="$HERE/../../.github/workflows/release.yml"
RDME="$HERE/../../README.md"
if [ ! -f "$RELYML" ]; then
  echo "FAIL: release asset names (release.yml not found at $RELYML)"; fail=1
elif [ ! -f "$RDME" ]; then
  echo "FAIL: release asset names (README.md not found at $RDME)"; fail=1
else
  # ABIs the Android job matrixes over — these substitute into the templated .apk asset name.
  rel_abis="$(sed -n '/^        include:/,/^    env:/p' "$RELYML" | awk '$1=="name:" && NF==2 {print $2}')"
  # Every filename handed to a release-attach step.
  rel_attached="$(awk '
    /uses: softprops\/action-gh-release/            { inrel=1; next }
    inrel && /^[[:space:]]*files:[[:space:]]*\|/    { blk=1; next }
    inrel && blk {
      if ($0 ~ /^[[:space:]]*[A-Za-z_][A-Za-z0-9_-]*:/) { blk=0; inrel=0; next }
      gsub(/^[[:space:]]+|[[:space:]]+$/, ""); if ($0 != "") print
      next
    }
    inrel && /^[[:space:]]*files:[[:space:]]*[^|[:space:]]/ {
      sub(/^[[:space:]]*files:[[:space:]]*/, ""); print; inrel=0; next
    }
  ' "$RELYML")"
  rel_emitted="$rel_attached"
  for abi in $rel_abis; do
    rel_emitted="$rel_emitted"$'\n'"$(printf '%s\n' "$rel_attached" | sed "s/\${{ matrix\.name }}/$abi/g")"
  done
  # Drop anything still carrying an unexpanded expression — an unknown template can't be name-checked.
  rel_emitted="$(printf '%s\n' "$rel_emitted" | grep -v '\${' | sort -u)"
  rel_linked="$(grep -oE 'releases/latest/download/[A-Za-z0-9._+-]+' "$RDME" | sed 's#.*/##' | sort -u)"
  if [ -z "$(printf '%s' "$rel_emitted" | tr -d '[:space:]')" ]; then
    echo "FAIL: release asset names (no release-attach 'files:' entries parsed out of release.yml)"; fail=1
  elif [ -z "$(printf '%s' "$rel_linked" | tr -d '[:space:]')" ]; then
    echo "FAIL: release asset names (no releases/latest/download links found in README.md)"; fail=1
  else
    rel_bad=""
    for a in $rel_linked; do
      printf '%s\n' "$rel_emitted" | grep -qxF -- "$a" || rel_bad="$rel_bad  $a"$'\n'
    done
    if [ -n "$rel_bad" ]; then
      echo "README links these, but no release.yml job attaches them:"; printf '%s' "$rel_bad"
      echo "release.yml attaches:"; printf '%s\n' "$rel_emitted" | sed 's/^/  /'
      echo "FAIL: release asset names (a README download link points at an asset CI never publishes)"; fail=1
    else
      echo "$(printf '%s\n' "$rel_linked" | wc -l | tr -d ' ') README download link(s), all produced by release.yml"
      echo "PASS: release asset names"
    fi
  fi
fi
echo

# uitest.py UTF-8 output gate (issue #36). The UI-test harness is how every UI change gets verified, and the
# app's labels are full of non-ASCII: "▶ Play" on a detail view, "☁"/"＋"/"✎"/"✕"/"★" on the settings rows,
# emoji profile avatars, em-dashes in theme names, and media titles in any language. When uitest.py's stdout is
# REDIRECTED (a pipe, a file, subprocess capture — i.e. every automated caller) CPython falls back to the locale
# encoding, and printing any of those raised UnicodeEncodeError from encodings/cp1252.py. This gate pins the fix:
# with stdout forced to cp1252 the client must still emit the glyphs, byte-for-byte, losing nothing. No app, no
# display, no network — it stubs the pipe with the exact bytes UiTestServer would write.
# The python below is deliberately pure ASCII (\u escapes, ASCII comments): it is fed to the interpreter on
# STDIN, and a CI box running under the C/POSIX locale would otherwise choke decoding this file's own glyphs.
echo "=== uitest utf-8 output ==="
utf8_out="$("$PY" - "$HERE/uitest.py" <<'PYEOF' 2>&1
import importlib.util, io, json, sys

spec = importlib.util.spec_from_file_location("uitest", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

payload = {
    "panelFocus":      "\U00002601   Restore from Google Drive",  # onboarding row, CLOUD
    "focusText":       "\U000025b6  Play",                        # the detail view's Play button
    "themedSelection": "\U0000ff0b  Create New Profile",          # FULLWIDTH PLUS
    "avatar":          "\U0001f3ae",                              # emoji profile icon (non-BMP)
    "theme":           "Lumen \U00002014 Dark",                   # em-dash in a theme name
    "glyphs":          "\U0000270e \U00002715 \U00002605",        # edit / delete / favourite
    "title":           "\U000030cf\U000030a4\U000030ad\U000030e5\U000030fc",  # a title in another script
}
wire = ("ok " + json.dumps(payload, ensure_ascii=False)).encode("utf-8")  # what UiTestServer puts on the pipe
m._send = lambda cmd: wire.decode("utf-8", "replace")

buf = io.BytesIO()
sys.stdout = io.TextIOWrapper(buf, encoding="cp1252", errors="strict")  # the pre-fix condition, on any OS
sys.argv = ["uitest.py", "state"]
rc = m.main()
sys.stdout.flush()
data = buf.getvalue()          # read BEFORE dropping the wrapper: collecting it closes the BytesIO
sys.stdout = sys.__stdout__

if rc != 0:
    raise SystemExit("uitest.py state returned %r" % rc)
got = json.loads(data.decode("utf-8"))  # decodes only if the bytes really are UTF-8
if got != payload:
    raise SystemExit("round-trip LOST characters: %r" % (got,))
print("UITEST-UTF8-OK")
PYEOF
)"
if printf '%s' "$utf8_out" | grep -q "UITEST-UTF8-OK"; then
  echo "PASS: uitest utf-8 output"
else
  printf '%s\n' "$utf8_out"
  echo "FAIL: uitest utf-8 output (a non-ASCII app label did not survive uitest.py's stdout)"
  fail=1
fi
echo

# Probe data-dir isolation WIRING gate (issue #42). The isolation itself is asserted at runtime by
# probe_isolation — delete the CMake block and that probe goes red. What a probe cannot see is the other
# direction: that the define is applied by NAME PATTERN over probe targets and to nothing else. A
# `target_compile_definitions(everythingbox PRIVATE EB_ISOLATED_DATA_DIR)` typed by mistake would send the
# shipped app's ini, saves and add-ons to a temp directory that deletes itself on exit — the loudest possible
# data-loss bug, and one no probe in this suite would notice because probes never build the app target.
echo "=== probe data-dir isolation wiring ==="
ISO_ROOT="$(cd "$HERE/.." && pwd)"
ISO_CMAKE="$ISO_ROOT/CMakeLists.txt"
if [ ! -f "$ISO_CMAKE" ]; then
  echo "FAIL: probe data-dir isolation wiring (CMakeLists.txt not found at $ISO_CMAKE)"; fail=1
else
  iso_bad=0
  # CMake comments stripped FIRST, and this is load-bearing rather than tidiness: the block being checked is
  # introduced by a comment that names BUILDSYSTEM_TARGETS, so a gate reading the raw file would go on passing
  # after someone deleted the sweep and left the prose describing it. That is precisely how an assertion ends
  # up gating nothing, and this one did until the mutation pass caught it.
  iso_src="$(sed -E 's/#.*$//' "$ISO_CMAKE")"
  # COUNTS, not `grep -q`, on this whole-file blob. This suite runs under `set -o pipefail`, and grep -q exits on
  # the FIRST match — which SIGPIPEs the printf feeding it once native/CMakeLists.txt (now ~160 KB, and only
  # growing) overruns the pipe buffer, so a SUCCESSFUL match surfaces as a failed pipeline and this gate flakes
  # red for no reason. grep -c reads to EOF and never closes the pipe early. See the note above the metadata-editor gate.
  iso_has() { [ "$(printf '%s\n' "$iso_src" | grep -c "$1" || true)" != "0" ]; }
  # The auto-apply loop must still be there, still keyed on the probe_ name prefix. Without it a new probe is
  # silently un-isolated, which is the trap this issue was about rather than a fix for it.
  iso_has 'BUILDSYSTEM_TARGETS' \
    || { echo "  the BUILDSYSTEM_TARGETS sweep is gone — isolation is no longer applied to every probe"; iso_bad=1; }
  iso_has '_eb_t MATCHES "\^probe_"' \
    || { echo "  the ^probe_ name match is gone — a new probe no longer gets isolation by default"; iso_bad=1; }
  iso_has 'target_compile_definitions(\${_eb_t} PRIVATE EB_ISOLATED_DATA_DIR)' \
    || { echo "  the loop no longer applies EB_ISOLATED_DATA_DIR — nothing is isolated"; iso_bad=1; }
  # Every grant of the define must go through the loop variable. Anything else names a target explicitly.
  iso_grants="$(printf '%s\n' "$iso_src" | grep -n 'target_compile_definitions(.*EB_ISOLATED_DATA_DIR' || true)"
  iso_by_name="$(printf '%s\n' "$iso_grants" | grep -v 'target_compile_definitions(\${_eb_t} ' | grep . || true)"
  if [ -n "$iso_by_name" ]; then
    printf '%s\n' "$iso_by_name"
    echo "  EB_ISOLATED_DATA_DIR is granted to a NAMED target above. It may only be applied through the"
    echo "  probe_ name sweep; on a non-probe target it redirects the shipped app's data dir at a"
    echo "  self-deleting temp folder."
    iso_bad=1
  fi

  # ---- One sanctioned site, everything else is a failure ---------------------------------------------------
  # Everything above is a BLACKLIST: it matches the one spelling of a bad grant whose comment names it, on a
  # single line, of a single file. The catastrophic mistake has plenty of other spellings, and every one of
  # them walks straight past those greps:
  #   * a MULTI-LINE invocation — `target_compile_definitions(everythingbox` on one line and
  #     `PRIVATE EB_ISOLATED_DATA_DIR)` on the next. The greps are line-based, so neither line matches both
  #     halves of the pattern and the by-name check comes up empty;
  #   * `add_compile_definitions(EB_ISOLATED_DATA_DIR)` — DIRECTORY scope, so it hits every target declared in
  #     the file, the app included. This is the realistic accident: someone debugging drops it near the top
  #     "temporarily". Every probe still passes (they are isolated either way), and the shipped app writes its
  #     ini, saves and add-ons into a folder that deletes itself on exit;
  #   * `set_property(TARGET everythingbox APPEND PROPERTY COMPILE_DEFINITIONS EB_ISOLATED_DATA_DIR)`;
  #   * a grant through a variable — `set(_defs EB_ISOLATED_DATA_DIR)` … `PRIVATE ${_defs}`;
  #   * a grant in ANY OTHER CMake file. cmake/GenerateSecrets.cmake and the two third_party subdirectory
  #     lists were never read at all.
  # So invert it. Scan every CMakeLists.txt and *.cmake under native/, strip comments, and treat ANY
  # occurrence of the EB_ISOLATED_DATA_DIR token outside the ONE sanctioned line as a failure — the sweep's
  # own `target_compile_definitions(${_eb_t} PRIVATE EB_ISOLATED_DATA_DIR)`, which must appear exactly once.
  # A whitelist of one known-good site is far more robust here than a blacklist of spellings: it needs no
  # updating when CMake grows another way to say it, or when someone invents a way nobody here thought of.
  ISO_OK_LINE='target_compile_definitions\(\$\{_eb_t\} PRIVATE EB_ISOLATED_DATA_DIR\)'
  iso_stray=""
  iso_ok_hits=0
  iso_scanned=0
  # Build trees are pruned: a configured build directory is full of GENERATED cmake fragments that legitimately
  # echo every target's compile definitions back at us (CMakeFiles/*/DependInfo.cmake and friends), and those
  # are output, not source. No source directory under native/ is named build*.
  while IFS= read -r f; do
    iso_scanned=$((iso_scanned + 1))
    iso_hits="$(sed -E 's/#.*$//' "$f" | grep -n 'EB_ISOLATED_DATA_DIR' || true)"
    [ -n "$iso_hits" ] || continue
    if [ "$f" = "$ISO_CMAKE" ]; then
      iso_ok_hits=$(( iso_ok_hits + $(printf '%s\n' "$iso_hits" | grep -cE "^[0-9]+:[[:space:]]*${ISO_OK_LINE}[[:space:]]*$" || true) ))
      iso_hits="$(printf '%s\n' "$iso_hits" | grep -vE "^[0-9]+:[[:space:]]*${ISO_OK_LINE}[[:space:]]*$" || true)"
    fi
    [ -n "$iso_hits" ] && iso_stray="$iso_stray"$'\n'"  $f"$'\n'"$(printf '%s\n' "$iso_hits" | sed 's/^/    /')"
  done < <(find "$ISO_ROOT" \( -name CMakeFiles -o -name _deps -o -name 'build*' -o -name .git \) -prune -o \
                            \( -name CMakeLists.txt -o -name '*.cmake' \) -print)
  if [ "$iso_scanned" -eq 0 ]; then
    echo "  no CMake files were scanned under $ISO_ROOT — this gate is reading nothing"; iso_bad=1
  fi
  if [ -n "$iso_stray" ]; then
    printf '%s\n' "$iso_stray"
    echo "  EB_ISOLATED_DATA_DIR appears above, outside the probe_ name sweep in native/CMakeLists.txt."
    echo "  That define may ONLY be granted by that sweep. Anywhere else — a named target, a directory-scope"
    echo "  add_compile_definitions(), a set_property(TARGET ...), a variable, another CMake file — it can"
    echo "  reach the shipped app, and then the app's ini, saves and add-ons go to a self-deleting temp folder."
    iso_bad=1
  fi
  # Exactly once: the sanctioned line is only safe because ${_eb_t} is the sweep's loop variable. Pasted a
  # second time, somewhere else, with _eb_t left holding a different target, it grants exactly what the
  # by-name check above exists to stop — and it would be whitelisted by the scan.
  if [ "$iso_ok_hits" -ne 1 ]; then
    echo "  the sweep's own grant appears $iso_ok_hits time(s) in native/CMakeLists.txt; it must appear exactly once"
    iso_bad=1
  fi

  if [ "$iso_bad" -eq 0 ]; then echo "PASS: probe data-dir isolation wiring"; else
    echo "FAIL: probe data-dir isolation wiring"; fail=1
  fi
fi
echo

# Bundled-theme / community-registry drift gate (issue #57). Channels, Night and Triple exist TWICE: bundled
# under native/themes2, and published in the community registry (github.com/cubman3134/everythingbox-themes)
# that the Appearance panel sends users to. Nothing noticed when a bundled theme gained a view and its
# registry twin did not, so both copies had quietly rotted: the registry's Channels had lost the
# nowplayingAudio view, the `channels` browse layout and the detail actionrow, and its Triple had lost
# everything but `home` — the exact blank-browse shape issue #29 was about. Under the same name, the
# downloadable theme was strictly worse than the shipped one.
#
# This gate cannot LOOK at the registry: it is a different repo, and this suite is offline on purpose (no
# network, no keys — that is what makes it a gate rather than a flaky test). So the comparison is against a
# checked-in record of what the registry is expected to be serving, native/themes2/REGISTRY-SYNC.json. Edit
# a bundled theme and its canonical hash moves; this goes red and names the theme and the command that
# refreshes the record.
#
# Be clear about what that buys, because for a year it bought less than it looked like (issue #151).
# --update recomputes the record from the BUNDLED theme and has never touched the registry, so it could be
# run by someone who never pushed — and was: #57 and #32 ran it alone, so the record asserted the registry
# was current while it still served the pre-#29 Triple, `home` and nothing else, under a green gate. The
# gate was not broken. What it compared against was a claim nobody had to substantiate.
#
# So the record now has to NAME what it was published against, and this prints that on every run, whichever
# it is: a registry commit sha (falsifiable anywhere with a network call — theme-registry-sync.py
# --verify-registry) or a written reason it has none. A bare --update is refused. Nothing here can PROVE the
# registry is current — this suite is offline and stays that way — but it no longer silently implies it.
#
# The COPY itself is no longer a hand step, which is what closes the loop the two paragraphs above describe:
# on a push to main touching native/themes2/**, the publish-themes workflow checks the registry out with a
# deploy key, runs --publish into it, pushes, and then rewrites this record with --update --registry-commit
# <the sha it just pushed> and pushes that back here. So the claim the gate prints is normally MACHINE
# written, and --assume-published is what a human writes when they are recording an intent instead. The
# verify-registry workflow re-reads the registry weekly, catching what the publisher cannot see: a direct
# edit there, a revert, or a publish that never ran. REGISTRY-SYNC.json spells that out.
#
# The hash is canonical, not byte-for-byte, so a reindent is not drift — see theme-registry-sync.py. The
# script also fails on a theme.json that stops parsing, a view declared with an empty `elements` (which the
# engine treats as not declared at all, so it silently falls back), and a bundled theme that is tracked in
# neither publishedThemes nor notPublished — the last one because otherwise the cheapest way to silence a
# red gate is to delete the offending entry.
echo "=== bundled-theme / registry drift ==="
THEMESYNC_PY="$HERE/theme-registry-sync.py"
if [ ! -f "$THEMESYNC_PY" ]; then
  echo "FAIL: bundled-theme / registry drift (theme-registry-sync.py not found at $THEMESYNC_PY)"; fail=1
elif "$PY" "$THEMESYNC_PY" --check; then
  echo "PASS: bundled-theme / registry drift"
else
  echo "FAIL: bundled-theme / registry drift — a bundled theme has moved away from the record of what the"
  echo "  community registry serves under the same name, or the record no longer says what it was published"
  echo "  against. Rerun with --update --assume-published \"<why>\" (or --registry-commit <sha> if you pushed"
  echo "  by hand), commit the refreshed record with the theme change; the publish workflow does the copy on"
  echo "  merge and rewrites the claim with the real sha (details above)."
  fail=1
fi
echo

# Registry index / manifest agreement rule (issue #151). The registry serves SEVEN themes; only three are
# bundled here, so the drift gate above cannot see Default, Grid, Lumen or Midnight at all — nothing checked
# that they parse, declare a usable view, or agree with the gallery card that advertises them. That gap is
# why index.json credited Triple to `cubman3134` while its own theme.json said `EverythingBox` for as long
# as both files existed: the card and the installed theme disagreed and no reader ever held them together.
#
# The RULE lives here, in theme-registry-validate.py, because this repo defines what index.json's fields
# mean (formFactors semantics in ThemeFormFactors.h, the field list in themes2/THEME_FORMAT.md). The
# registry's CI downloads and runs it, the same way its theme-assets.yml downloads the app's Theme.js
# instead of keeping a second copy of that rule.
#
# This suite has no registry checkout and no network, so it cannot run the rule against the real data. What
# it runs is --selftest: build a synthetic registry, confirm the correct one passes, then break one thing at
# a time and require the matching complaint — plus three negative controls, since a rule that fires on
# everything is as useless as one that fires on nothing. A validator nobody has shown can fail is the same
# defect as a sync record nobody has to substantiate, one level up.
#
# It also covers the validator's could-not-run branches, and those assert the EXIT STATUS, not just the
# complaint. They are the family that matters most to the registry's CI: a permissive edit to a per-theme
# check lets one bad submission through, but a permissive edit to "there is no index.json here" makes the
# whole file pass on anything it is pointed at — and running --selftest on the downloaded copy is the only
# thing standing between that file and a green verdict on somebody's PR.
echo "=== registry index / manifest rule ==="
REGVALIDATE_PY="$HERE/theme-registry-validate.py"
if [ ! -f "$REGVALIDATE_PY" ]; then
  echo "FAIL: registry index / manifest rule (theme-registry-validate.py not found at $REGVALIDATE_PY)"; fail=1
elif "$PY" "$REGVALIDATE_PY" --selftest; then
  echo "PASS: registry index / manifest rule"
else
  echo "FAIL: registry index / manifest rule — a check in theme-registry-validate.py cannot be shown to fire"
  echo "  on the defect it names, so the registry's CI would be running a rule that reports nothing."
  fail=1
fi
echo

# Mutation-driver rule (issue #175). Every assertion in this suite is supposed to be proven by breaking the
# behaviour it guards and watching the probe go red. mutate.py is the one driver that does that loop, and it
# is gated here for the same reason theme-registry-validate.py is: it is a tool whose entire job is to not
# lie about a test result, so a version of it that cannot be shown to discriminate is worse than no version.
#
# What it must still discriminate is three outcomes, not two. KILLED and SURVIVED are the ones people think
# about; NOT APPLIED is the one that caused the issue. An unapplied mutation looks EXACTLY like a surviving
# one from outside — the test passes, because the code under it never changed — and a driver that collapses
# them reports SURVIVED, which reads as "this assertion is inert" and gets a working assertion deleted. That
# happened three times in one day (#123, #151, #164), every time because a multi-line anchor was written
# with "\n" against this CRLF tree, and every time to someone who had read the warning.
#
# --selftest drives real matrices against a throwaway CRLF subject and requires: a "\n" anchor spanning a
# line break APPLIES (and so do "\r\n" anchors); a mutation the test does not cover reports SURVIVED; and a
# drifted anchor, an ambiguous anchor, a no-op edit, a build that did not rebuild, a build that failed, and
# a test that exited 0 without its sentinel each report NOT APPLIED rather than any verdict at all. It also
# checks the restore puts the bytes back AND advances the mtime — a restore that carries the backup's old
# timestamp back leaves MSBuild skipping the file, so a reverted tree keeps testing as mutated.
#
# It needs no compiler and no build: the "build" step copies a text file and the "test" step greps it.
echo "=== mutation driver rule ==="
MUTATE_PY="$HERE/mutate.py"
if [ ! -f "$MUTATE_PY" ]; then
  echo "FAIL: mutation driver rule (mutate.py not found at $MUTATE_PY)"; fail=1
elif "$PY" "$MUTATE_PY" --selftest; then
  echo "PASS: mutation driver rule"
else
  echo "FAIL: mutation driver rule — mutate.py can no longer be shown to tell KILLED, SURVIVED and NOT"
  echo "  APPLIED apart. Until it can, every mutation result quoted in a PR is unfalsifiable: a mutation"
  echo "  that never applied reads as a surviving one, and that is the verdict that deletes a working"
  echo "  assertion. Re-run with --selftest --verbose for the captured matrices."
  fail=1
fi
echo

# Trakt import wiring gate (issue #23, review round 1). The RULES of the watched-history import are pure and
# probe_trakt pins them hard. Everything below is the thin impure layer between those rules and the user —
# a QSettings, a reply lambda, two settings builders — and NONE of it is reachable from a headless probe
# without a socket, an ini and a themed panel host. That is exactly where the defects this round fixed were
# living, so the shape of the wiring is gated here rather than left held by nothing.
#
# Each check corresponds to a defect that was real:
#   * the WATERMARK was stored at one flat device-wide key while the marks it gates land per profile, so the
#     second profile to import got no history and no way to ask for one;
#   * a run truncated at kMaxPages reported COMPLETE, caching a partial list as whole;
#   * the only escape from the watermark has to exist in BOTH settings builders, or one of the two modes has
#     no way out at all.
echo "=== trakt import wiring ==="
TKC="$HERE/../src/core/TraktClient.cpp"
TKM="$HERE/../src/ui/MainWindow.cpp"
tk_fail=0
tk_note() { echo "  $1"; tk_fail=1; }
if [ ! -f "$TKC" ] || [ ! -f "$TKM" ]; then
  echo "FAIL: trakt import wiring (TraktClient.cpp or MainWindow.cpp not found)"; fail=1
else
  # Comments stripped into temp files; both sources DISCUSS the flat legacy keys and the old behaviour at
  # length, and prose naming them must never trip a gate about what the code does.
  #
  # FILES and `grep -c`, never `printf ... | grep -q`. On an input this size grep -q matches, exits, and
  # SIGPIPEs the printf — and under this script's `set -o pipefail` the pipeline then reports FAILURE for
  # a SUCCESSFUL match. A gate that goes red exactly when the thing it looks for is present is worse than
  # no gate; counting reads the whole input, so the status means what it says. (Found while writing this:
  # the first draft failed only on MainWindow.cpp, the one file big enough to lose the race.)
  tk_ctmp="$(mktemp)"; tk_mtmp="$(mktemp)"
  sed -E 's://.*$::' "$TKC" > "$tk_ctmp"
  sed -E 's://.*$::' "$TKM" > "$tk_mtmp"
  tk_has() { [ "$(grep -c -- "$2" "$1")" -gt 0 ]; }

  # 1. The cursor is written through the per-profile key builder, never as a literal. The two flat names may
  #    appear ONCE each, as the kLegacy* constants clearBackfillState removes.
  for tk_k in trakt/backfillThrough trakt/backfillDone; do
    tk_n="$(grep -c "\"$tk_k\"" "$tk_ctmp")"
    [ "$tk_n" -le 1 ] || tk_note "TraktClient.cpp names \"$tk_k\" $tk_n times: the live cursor must go through trakt::backfillThroughKey(profileId), or one profile's import silences another's."
  done
  #    ...and the kLegacy* CONSTANTS are referenced exactly twice each — their declaration, and the one
  #    removal in clearBackfillState. A third reference means something reads or writes a flat key again,
  #    which the literal count above cannot see because it would go through the constant.
  for tk_k in kLegacyBackfillThroughKey kLegacyBackfillDoneKey; do
    tk_n="$(grep -c "$tk_k" "$tk_ctmp")"
    [ "$tk_n" -eq 2 ] || tk_note "$tk_k is referenced $tk_n times (expected 2: its declaration and the removal in clearBackfillState). A live read or write of the flat key un-does the per-profile scoping."
  done
  tk_has "$tk_ctmp" 'trakt::backfillThroughKey(profileId)' \
    || tk_note "TraktClient.cpp never reads trakt::backfillThroughKey(profileId) — the watermark is not profile-scoped."
  tk_has "$tk_ctmp" 'store().remove(trakt::backfillKeyPrefix()' \
    || tk_note "disconnectAccount does not remove the whole trakt/backfill group: a profile's cursor would outlive the account it describes."

  # 2. A run is COMPLETE in exactly one place, and it is the LastPage branch. This is the dead-guard defect:
  #    when 'bound hit' and 'last page' shared one answer, a truncated run set complete = true.
  tk_done="$(grep -c 'run->complete = true;' "$tk_ctmp")"
  [ "$tk_done" -eq 1 ] || tk_note "TraktClient.cpp sets run->complete in $tk_done places (expected 1): only PageStep::LastPage may mean the run read everything."
  for tk_s in LastPage BoundHit Unusable Next; do
    tk_has "$tk_ctmp" "PageStep::$tk_s" \
      || tk_note "the fetch loop does not handle PageStep::$tk_s — an unhandled step is a run whose outcome is decided by a default."
  done

  # 3. The surface. The import is scoped to the profile that started it, the run is abandoned if that
  #    changes, and the re-import escape hatch + the status line exist in BOTH builders.
  tk_has "$tk_mtmp" 'ProfileStore::currentId() == profileId' \
    || tk_note "MainWindow does not guard the run against a mid-run profile switch (ProfileStore::currentId() == profileId): the plan would be computed against one profile's marks and written into another's."
  #    Each builder is named by a marker only IT can produce — a themed row id, or the QWidget the classic
  #    panel builds — rather than by a count of references, which a mutation can satisfy while leaving one
  #    builder without the row (both survived that weaker form).
  tk_has "$tk_mtmp" 'action(QStringLiteral("trakt.reimport")' \
    || tk_note "the THEMED settings builder has no \"Re-import everything\" row: in that mode the watermark has no escape at all."
  tk_has "$tk_mtmp" 'QStringLiteral("trakt.reimport")) { reimportTraktHistory' \
    || tk_note "the themed builder's trakt.reimport row is not wired to reimportTraktHistory() — a row that does nothing."
  tk_has "$tk_mtmp" 'new QPushButton(tr("Re-import everything from Trakt"))' \
    || tk_note "the CLASSIC settings builder has no \"Re-import everything\" button: a user-facing action must exist in BOTH builders or it is unreachable in one of the two modes."
  tk_has "$tk_mtmp" 'reimportTraktHistory(); });' \
    || tk_note "the classic builder's re-import button is not wired to reimportTraktHistory()."
  tk_has "$tk_mtmp" 'info(QStringLiteral("trakt.data")' \
    || tk_note "the THEMED settings builder has no Trakt status row: the import's watermark is invisible there, and \"0 newly marked\" is then indistinguishable from \"nothing to do\"."
  tk_has "$tk_mtmp" 'new QLabel(traktStatusLine())' \
    || tk_note "the CLASSIC settings builder does not show traktStatusLine(): same invisibility, other mode."
  # 4. "You missed" (issue #25). The selection RULE is pure and probe_trakt pins every clause of it; what
  #    no probe can reach is whether the app ever asks Trakt for the window that rule selects over.
  #
  #    daysBack was 0 before this feature, with a comment saying past episodes were #25's job. If it goes
  #    back to a literal — or to any number that is not the constant the rule uses — the surface is EMPTY,
  #    permanently, on every install, and the entire suite stays green: the rule would be selecting over a
  #    span nothing had ever fetched, and there is no assertion anywhere that could tell the difference
  #    between "you have missed nothing" and "we never looked". That is the exact shape of defect this gate
  #    family exists for, so the fetch is required to name the constant rather than a number.
  #    (Matched as a BRE against the call itself. A literal `/*daysBack*/` in the pattern would NOT work —
  #    `/*` is "zero or more slashes" to grep, not the comment it looks like, which is how the first draft
  #    of this check failed against the very line it was written from.)
  tk_has "$tk_mtmp" 'fetchMyShowsCalendar(.*trakt::kMissedLookbackDays' \
    || tk_note "refreshTraktCalendar does not fetch trakt::kMissedLookbackDays days BACK: the \"You missed\" rule would select over a window the app never requested, and the surface would be silently empty for ever."
  TKH="$HERE/../src/ui/HomeView.cpp"
  if [ ! -f "$TKH" ]; then
    tk_note "HomeView.cpp not found — the \"You missed\" surface checks could not run."
  else
    tk_htmp="$(mktemp)"; sed -E 's://.*$::' "$TKH" > "$tk_htmp"
    #  The Trakt-off rule: every surface in this feature is gated on the account being configured AND
    #  connected, so an install that never linked Trakt grows no shelf, no folder and no empty header.
    #  Asserted on the builder itself, because it is the one function both surfaces go through.
    tk_has "$tk_htmp" 'MediaCatalog HomeView::traktMissedItems(int maxRows) const' \
      || tk_note "HomeView::traktMissedItems is gone or has changed shape — the two \"You missed\" surfaces no longer share one gate."
    tk_hn="$(grep -c 'TraktClient::calendarAvailable()) return MediaCatalog{};' "$tk_htmp")"
    [ "$tk_hn" -ge 3 ] || tk_note "only $tk_hn of the three Trakt catalog builders re-assert calendarAvailable(): an install that never linked Trakt must get no shelf and no folder from ANY of them."
    #  The shelf is BOUNDED and the folder is not. A shelf that grows with how long the user has been away
    #  pushes everything below it off a TV screen; a folder that is capped hides the backlog it exists to
    #  show. Both halves are checked, because getting either one wrong looks fine from the other.
    tk_has "$tk_htmp" 'traktMissedItems(trakt::kMissedShelfMax)' \
      || tk_note "the Home \"You Missed\" shelf is not capped at trakt::kMissedShelfMax — its length would be set by how long the user has been away."
    tk_has "$tk_htmp" 'showSyntheticCatalog(traktMissedItems(0))' \
      || tk_note "the \"You Missed\" FOLDER is capped: it is where the whole backlog is dealt with, so it must pass 0 (uncapped)."
    #  The dismissal reads its target through the marker readers, never by slicing the mime by hand. Two
    #  parsers for one format is how the show key and the stamp come to disagree, and the failure is silent:
    #  the press appears to work and the row returns on the next rebuild.
    tk_has "$tk_htmp" 'browse::traktMissedShowKeyOf(it.mime)' \
      || tk_note "HomeView does not read the dismissal target through browse::traktMissedShowKeyOf — a hand-rolled parse is a second opinion about the marker format."
    tk_has "$tk_htmp" 'browse::traktMissedThroughOf(it.mime)' \
      || tk_note "HomeView does not read the dismissal stamp through browse::traktMissedThroughOf — same reason."
    tk_has "$tk_htmp" 'MissedDismiss::dismissThrough(showKey, through)' \
      || tk_note "the \"I'm caught up\" row does not write a dismissal — a menu row that does nothing."
    rm -f "$tk_htmp"
  fi
  rm -f "$tk_ctmp" "$tk_mtmp"
fi
if [ "$tk_fail" -eq 0 ]; then echo "PASS: trakt import wiring"; else echo "FAIL: trakt import wiring"; fail=1; fi
echo

# General settings builder parity gate (issue #133). CONTRIBUTING's two-builder rule — a user-facing setting
# must exist in BOTH halves of MainWindow::openGeneralSettings(), because the themed surface is the
# default-reachable one but a user who never enables the themed home picks settings ONLY in the classic form —
# has now been paid for three times in one week. #40 and #47 were its D-pad shape (a control that exists but
# cannot be reached); #133 is its settings shape: "Match local files to online catalogs" and "Re-match Local
# Library online" shipped themed-only, so the classic surface could point at a library folder and rescan it but
# could neither turn online matching on nor ask for it again. "Show hidden items" was a third, found by the
# sweep this gate replaces.
#
# The rule rots SILENTLY by construction: both builders compile, both surfaces look complete on their own, and
# until now nothing anywhere compared them. The comparison is mechanical — the themed builder names every row
# with a string id, so the ids ARE the setting list — so it is done here, at commit time, instead of by a person
# noticing.
#
# What is gated: the CONTROL rows — toggle/action/textf/choice. Separator and Info rows are section headings and
# status read-outs (chrome, not capability), and requiring a twin for each would gate prose rather than
# reachability. Every defect in this class so far (#40, #47, #133) was "a control exists in one surface and not
# the other", which is exactly the set below.
#
# Direction is BOTH ways. Forward: every themed control id must name a classic twin in the table, and that
# twin's construction must be present. Reverse: every control the classic builder constructs must be claimed by
# some table entry, or be listed as classic-only WITH ITS REASON. An unexplained exemption is indistinguishable
# from an oversight, so both exemption lists are checked for staleness too: an exemption that matches nothing
# fails just as loudly as a missing row.
#
# Two traps the gates above already hit and paid for, honoured here:
#   * COMMENTS ARE STRIPPED FIRST. Both builders discuss their twins at length ("the twin of the themed
#     builder's trakt.reimport row"), and a token named in the prose that introduces a block would satisfy the
#     match with the code underneath it deleted.
#   * THE CORPUS IS ASSERTED NON-EMPTY, and counted over FILES rather than piped. A gate that scans nothing
#     prints PASS, which is worse than having no gate; and `printf … | grep -q` on a source blob this size
#     SIGPIPEs the printf under this script's `set -o pipefail`, which is how one earlier gate came to HANG
#     rather than report. Every match below is `grep -c -F` against a temp FILE.
#
# Rows that exist in ONE builder deliberately — the exemptions, with their reasons:
#   * classic `rPath` / `llPath` / `phPath` / `muPath` QLineEdits — read-only path DISPLAYS, not controls.
#     Their themed twins are the `roms.path` / `library.path` / `photos.path` / `music.path` Info rows, and
#     Info rows are out of this gate's scope (above). The classic controls that change those paths (Change…)
#     are twinned normally.
#   * classic `edit = new QLineEdit(value)` — the body of the addCredRow() factory, which builds five different
#     credential fields. The five are each twinned through their own addCredRow(…) call site, so the factory
#     itself has no id of its own to match.
#   * classic `sSave = panelRow(tr("Save Steam Key + SteamID"))` — classic-only BY DESIGN. The themed steam.key
#     and steam.steamid TextField rows commit on edit, so there is no Save action there for this to twin; it
#     exists because the classic form's QLineEdits do not write until something tells them to.
#   * themed-only: NONE. Every themed control row currently has a classic twin, which is the point of the fix
#     this gate defends. The list below is empty on purpose, not missing.
echo "=== general settings builder parity ==="
GSM="$HERE/../src/ui/MainWindow.cpp"
gs_fail=0
gs_note() { echo "  $1"; gs_fail=1; }
if [ ! -f "$GSM" ]; then
  echo "FAIL: general settings builder parity (MainWindow.cpp not found at $GSM)"; fail=1
else
  # id|pattern. The pattern is the CONSTRUCTION the classic builder must contain for that themed row, matched
  # as a fixed string (grep -F) so parentheses, quotes and ellipses need no escaping. Where two classic controls
  # carry the same label (the two "Change…" buttons), the pattern is qualified by the variable so the gate
  # cannot be satisfied by the wrong one.
  GS_TWINS=(
    'disp.fullscreen|new QCheckBox(tr("Open in full screen on startup"))'
    'attract.enabled|new QCheckBox(tr("Play a screensaver slideshow when idle"))'
    'attract.timeout|attractTimeout = new QComboBox()'
    'lib.showhidden|new QCheckBox(tr("Show hidden items"))'
    'update.autocheck|new QCheckBox(tr("Check for updates on startup"))'
    'remote.enabled|new QCheckBox(tr("Control from a phone on your network"))'
    'update.check|new QPushButton(tr("Check now"))'
    'update.install|new QPushButton(tr("Install update"))'
    'livetv.add|tvAdd = panelRow(tr("Add a Live TV Source'
    'roms.change|rBrowse = new QPushButton(tr("Change…"))'
    'roms.open|panelRow(tr("Open ROMs Folder"))'
    'roms.keepscrape|new QCheckBox(tr("Keep scraped data in the ROMs folder'
    'roms.softpatch|new QCheckBox(tr("Auto-apply ROM patches'
    'roms.verify|new QCheckBox(tr("Verify ROMs against DAT files (No-Intro / Redump)"))'
    'roms.collapseregions|new QCheckBox(tr("Collapse regional duplicates"))'
    'roms.keepdownloads|new QCheckBox(tr("Keep downloaded games in the ROMs folder"))'
    'ps3.autoupdate|new QCheckBox(tr("Auto-install PS3 game updates"))'
    'emu.autoinc|new QCheckBox(tr("Quick-save to the next free slot (keeps a history)"))'
    'emu.resume|resumeMode = new QComboBox()'
    'emu.hardcore|new QCheckBox(tr("Hardcore RetroAchievements (no save states, rewind or cheats)"))'
    'emu.shaderpreset|shaderPreset = new QComboBox()'
    'emu.rpdriven|rpDriven = new QComboBox()'
    'library.change|llBrowse = new QPushButton(tr("Change…"))'
    'library.rescan|llRescan = new QPushButton(tr("Rescan"))'
    'library.resolveonline|new QCheckBox(tr("Match local files to online catalogs"))'
    'library.rematch|new QPushButton(tr("Re-match Local Library online"))'
    'library.clearmetaedits|new QPushButton(tr("Reset my metadata edits'
    'photos.change|phBrowse = new QPushButton(tr("Change…"))'
    'music.change|muBrowse = new QPushButton(tr("Change…"))'
    'music.rescan|muRescan = new QPushButton(tr("Rescan"))'
    'music.addserver|muSrvAdd = new QPushButton(tr("Add a music server'
    'music.separators|muSeps = new QLineEdit(Settings::musicTagSeparators())'
    'music.prefsource|muPref = new QComboBox()'
    'music.clearmatches|muClear = new QPushButton(tr("Reset my music match corrections'
    'audiobooks.change|abBrowse = new QPushButton(tr("Change…"))'
    'audiobooks.rescan|abRescan = new QPushButton(tr("Rescan"))'
    'books.change|bkBrowse = new QPushButton(tr("Change…"))'
    'books.rescan|bkRescan = new QPushButton(tr("Rescan"))'
    'pb.autonext|new QCheckBox(tr("Auto-play the next episode"))'
    'pb.gapless|new QCheckBox(tr("Gapless playback"))'
    'pb.replaygain|replayGain = new QComboBox()'
    'pb.rgpreamp|rgPreamp = new QComboBox()'
    'pb.crossfade|crossfade = new QComboBox()'
    'pb.onlinelyrics|new QCheckBox(tr("Look up lyrics online"))'
    'pb.defaultspeed|defSpeed = new QComboBox()'
    'pb.jump|jumpBox = new QComboBox()'
    'pb.skipseg|new QCheckBox(tr("Skip intros and credits"))'
    'pb.skipsegauto|new QCheckBox(tr("Skip them automatically (no button)"))'
    'pb.hwdec|hwdec = new QComboBox()'
    'pb.refreshsync|new QCheckBox(tr("Reduce judder (sync video to display)"))'
    'pb.hdr|hdr = new QComboBox()'
    'player.external|player = new QComboBox()'
    'player.custompath|new QPushButton(tr("Choose custom program…"))'
    'pb.bezel|new QCheckBox(tr("Show bezel / border art around games"))'
    'pb.bezelopen|new QPushButton(tr("Open bezels folder"))'
    'audio.device|audioDev = new QComboBox()'
    'audio.passthrough|new QCheckBox(tr("Passthrough (bitstream to receiver)"))'
    'audio.exclusive|new QCheckBox(tr("Exclusive mode (bit-perfect)"))'
    'subs.on|new QCheckBox(tr("Show subtitles by default"))'
    'content.lang|lang = new QComboBox()'
    'subs.font|subFont = new QComboBox()'
    'subs.size|subSize = new QComboBox()'
    'subs.color|subColor = new QComboBox()'
    'subs.bordersize|subBorderSize = new QComboBox()'
    'subs.bordercolor|subBorderColor = new QComboBox()'
    'subs.box|new QCheckBox(tr("Show a background box behind subtitles"))'
    'subs.boxopacity|subBoxOpacity = new QComboBox()'
    'subs.pos|subPos = new QComboBox()'
    'subs.bold|new QCheckBox(tr("Bold subtitles"))'
    'subs.override|new QCheckBox(tr("Override styled (ASS/SSA) subtitles"))'
    'reader.font|readerFont = new QComboBox()'
    'reader.size|readerSize = new QComboBox()'
    'reader.spacing|readerSpacing = new QComboBox()'
    'reader.margin|readerMargin = new QComboBox()'
    'reader.justify|new QCheckBox(tr("Justify text"))'
    'reader.theme|readerTheme = new QComboBox()'
    'os.api|addCredRow(tr("API key:")'
    'os.user|addCredRow(tr("Username:")'
    'os.pass|addCredRow(tr("Password:")'
    'trakt.id|addCredRow(tr("Client ID:")'
    'trakt.secret|addCredRow(tr("Client secret:")'
    'trakt.connect|new QPushButton(TraktClient::connected() ? tr("Disconnect")'
    'trakt.backfill|new QPushButton(tr("Import watched history from Trakt"))'
    'trakt.reimport|new QPushButton(tr("Re-import everything from Trakt"))'
    'scrobble.on|new QCheckBox(tr("Scrobble music I listen to"))'
    'scrobble.lbtoken|addCredRow(tr("ListenBrainz token:")'
    'scrobble.lburl|addCredRow(tr("Custom API URL:")'
    'scrobble.spoken|new QCheckBox(tr("Also scrobble audiobooks and podcasts"))'
    'profiles.skipsingle|new QCheckBox(tr("Skip the profile picker when there'
    'parental.setpin|new QPushButton(Settings::hasParentalPin() ? tr("Change PIN")'
    'parental.clearpin|new QPushButton(tr("Remove PIN"))'
    'profile:|new QCheckBox((pr.icon.isEmpty()'
    'bgm.on|new QCheckBox(tr("Play background music"))'
    'bgm.vol|new QSlider(Qt::Horizontal)'
    'bgm.open|panelRow(tr("Open Music Folder"))'
    'video.previews|new QCheckBox(tr("Play video previews on hover"))'
    'video.snapvol|new QSlider(Qt::Horizontal)'
    'steam.key|sKey = new QLineEdit(Settings::steamWebApiKey())'
    'steam.steamid|sId = new QLineEdit(Settings::steamId())'
    'debrid.torbox|tbKey = new QLineEdit(store().value(QStringLiteral("debrid/torbox/apikey"))'
    'community.discord|panelRow(tr("Join the Discord"))'
    'community.patreon|panelRow(tr("Support on Patreon"))'
  )
  # Themed control ids with no classic twin ON PURPOSE (reasons in the block above). Empty today.
  GS_THEMED_ONLY=()
  # Classic control constructions with no themed twin ON PURPOSE (reasons in the block above).
  GS_CLASSIC_ONLY=(
    'rPath = new QLineEdit(Settings::romsFolder())'
    'llPath = new QLineEdit(Settings::libraryFolder())'
    'phPath = new QLineEdit(Settings::photosFolder())'
    'muPath = new QLineEdit(Settings::musicFolder())'
    'abPath = new QLineEdit(Settings::audiobookFolder())'
    'bkPath = new QLineEdit(Settings::readingFolder())'
    'edit = new QLineEdit(value)'
    'sSave = panelRow(tr("Save Steam Key + SteamID"))'
  )

  gs_src="$(mktemp)"; gs_themed="$(mktemp)"; gs_classic="$(mktemp)"; gs_ctrl="$(mktemp)"
  sed -E 's://.*$::' "$GSM" > "$gs_src"
  # Split openGeneralSettings in two at the classic builder's showPanel() call. Everything before it (the
  # `#ifdef EB_HAVE_QML` themed branch) is the themed builder; everything after, to the closing brace, is the
  # classic one. A file operand is always supplied so awk can never fall back to the suite's own stdin.
  awk -v T="$gs_themed" -v C="$gs_classic" '
    /^void MainWindow::openGeneralSettings\(\)/ { fn = 1 }
    fn && /showPanel\(tr\("General"\)/          { part = 2 }
    fn && part != 2                             { print > T; next }
    fn && part == 2                             { print > C }
    fn && part == 2 && /^\}/                    { fn = 0 }
  ' "$gs_src" </dev/null
  gs_tlines="$(wc -l < "$gs_themed" | tr -d '[:space:]')"
  gs_clines="$(wc -l < "$gs_classic" | tr -d '[:space:]')"

  # Every themed CONTROL row, by id. (The per-profile toggle is built as QStringLiteral("profile:") + pr.id, so
  # it lands here as the literal prefix "profile:" — which is what its table entry names.)
  grep -oE '^[[:space:]]*(toggle|action|textf|choice)\(QStringLiteral\("[^"]+"' "$gs_themed" \
    | sed -E 's/.*QStringLiteral\("//; s/"$//' | sort -u > "$gs_ctrl.ids"
  # Every control the classic builder CONSTRUCTS.
  grep -nE 'new (QCheckBox|QPushButton|QLineEdit|QComboBox|QSlider)\(|= panelRow\(' "$gs_classic" > "$gs_ctrl"
  gs_nids="$(wc -l < "$gs_ctrl.ids" | tr -d '[:space:]')"
  gs_nctrl="$(wc -l < "$gs_ctrl" | tr -d '[:space:]')"

  # Corpus assertions BEFORE any comparison: an empty or truncated corpus makes every check below vacuously
  # true, and this gate would then report a rule as enforced while enforcing nothing. Floors are well under
  # today's sizes (504/673 lines, 42 ids, 41 controls); a restructure that legitimately shrinks a builder
  # should move them deliberately, which is the point.
  [ "$gs_tlines" -ge 200 ] || gs_note "the themed builder region is $gs_tlines line(s) — expected the whole \`#ifdef EB_HAVE_QML\` branch of openGeneralSettings. The split markers moved; treat a PASS here as meaningless."
  [ "$gs_clines" -ge 300 ] || gs_note "the classic builder region is $gs_clines line(s) — expected the whole showPanel(tr(\"General\")) body. The split markers moved; treat a PASS here as meaningless."
  [ "$gs_nids"   -ge 40  ] || gs_note "found $gs_nids themed control row(s) — expected the full General list. Either the row-builder lambdas were renamed or the region split is wrong; this gate is now comparing almost nothing."
  [ "$gs_nctrl"  -ge 35  ] || gs_note "found $gs_nctrl classic control construction(s) — expected the full General form. This gate is now comparing almost nothing."

  gs_twin_for() {   # id -> its classic pattern; non-zero if the id is not in the table at all
    local e
    for e in "${GS_TWINS[@]}"; do
      case "$e" in "$1|"*) printf '%s' "${e#*|}"; return 0 ;; esac
    done
    return 1
  }
  gs_is_themed_only() {
    local e
    for e in ${GS_THEMED_ONLY[@]+"${GS_THEMED_ONLY[@]}"}; do [ "$e" = "$1" ] && return 0; done
    return 1
  }

  # --- Forward: themed -> classic. A themed control row must be twinned, or exempt. ---
  # Read from a file, never through a pipe: a `while` on the right of a pipe runs in a subshell and every
  # gs_note() it makes would be discarded with it, turning a red gate green.
  while IFS= read -r gs_id; do
    [ -n "$gs_id" ] || continue
    if gs_is_themed_only "$gs_id"; then continue; fi
    if gs_pat="$(gs_twin_for "$gs_id")"; then
      [ "$(grep -c -F -- "$gs_pat" "$gs_classic")" -gt 0 ] \
        || gs_note "themed row \"$gs_id\" has no classic twin: the CLASSIC builder never constructs \`$gs_pat\`. A user who has not enabled the themed home cannot reach this setting at all. Add it to the classic form (or, if it is deliberately themed-only, to GS_THEMED_ONLY with its reason)."
    else
      gs_note "themed row \"$gs_id\" is not in this gate's twin table. A new user-facing row was added to the themed builder without declaring what the CLASSIC builder does about it — add its classic construction to GS_TWINS, or list it in GS_THEMED_ONLY with a reason."
    fi
  done < "$gs_ctrl.ids"

  # --- Reverse: classic -> themed. A classic control must be claimed by a twin entry, or be exempt. ---
  while IFS= read -r gs_line; do
    [ -n "$gs_line" ] || continue
    gs_hit=0
    for gs_e in "${GS_TWINS[@]}"; do
      case "$gs_line" in *"${gs_e#*|}"*) gs_hit=1; break ;; esac
    done
    if [ "$gs_hit" -eq 0 ]; then
      for gs_e in ${GS_CLASSIC_ONLY[@]+"${GS_CLASSIC_ONLY[@]}"}; do
        case "$gs_line" in *"$gs_e"*) gs_hit=1; break ;; esac
      done
    fi
    [ "$gs_hit" -eq 1 ] \
      || gs_note "classic control is claimed by nothing: ${gs_line#*:}. It has no themed twin in the table, so a user on the themed home cannot reach it. Add the themed row and its GS_TWINS entry, or list the construction in GS_CLASSIC_ONLY with a reason."
  done < "$gs_ctrl"

  # --- Staleness. An exemption that matches nothing has outlived what it excused, and a table entry for a row
  #     that no longer exists quietly stops asserting anything. Both read as "documented" until checked. ---
  for gs_e in ${GS_THEMED_ONLY[@]+"${GS_THEMED_ONLY[@]}"}; do
    [ "$(grep -c -F -x -- "$gs_e" "$gs_ctrl.ids")" -gt 0 ] \
      || gs_note "GS_THEMED_ONLY names \"$gs_e\", which is not a themed control row. A stale exemption reads exactly like a documented decision — remove it."
    gs_twin_for "$gs_e" >/dev/null \
      && gs_note "\"$gs_e\" is BOTH in GS_TWINS and exempt as themed-only. It cannot be both twinned and deliberately absent; delete one."
  done
  for gs_e in ${GS_CLASSIC_ONLY[@]+"${GS_CLASSIC_ONLY[@]}"}; do
    [ "$(grep -c -F -- "$gs_e" "$gs_ctrl")" -gt 0 ] \
      || gs_note "GS_CLASSIC_ONLY excuses \`$gs_e\`, which the classic builder no longer constructs. A stale exemption reads exactly like a documented decision — remove it."
  done
  for gs_e in "${GS_TWINS[@]}"; do
    [ "$(grep -c -F -x -- "${gs_e%%|*}" "$gs_ctrl.ids")" -gt 0 ] \
      || gs_note "GS_TWINS maps themed row \"${gs_e%%|*}\", which no longer exists. Either the row was removed (drop the entry, and check the classic twin went with it) or its id changed (the gate is asserting nothing about it)."
  done

  rm -f "$gs_src" "$gs_themed" "$gs_classic" "$gs_ctrl" "$gs_ctrl.ids"
  if [ "$gs_fail" -eq 0 ]; then
    echo "PASS: general settings builder parity ($gs_nids themed control rows, $gs_nctrl classic controls)"
  else
    echo "FAIL: general settings builder parity — the two builders of openGeneralSettings() have drifted."; fail=1
  fi
fi
echo

# Themed local-leaf routing parity. A leaf's Enter reaches playback down TWO dispatch paths: the classic grid
# calls HomeView::activateItem, and the themed (Triple/XMB) column opens an inline chooser whose Play calls
# HomeView::playThemedLeaf. Both must answer "is this row a local file, which no addon can resolve?" the same
# way, and until browse::LeafRoute existed both answered it from a list written out by hand — two lists, which
# had already drifted three ways. A kind in one and not the other falls through to resolvePlay, which has no
# local branch, and tells the user "Nothing to play" for a row the OTHER layout plays perfectly. That shipped
# for a music track (#74), a photo (#102) and an OPDS book (#146), and nothing in the suite could see it.
#
# The rule this enforces: neither surface has a list. Both read browse::localLeafRoute, every route it can
# return is handled on BOTH, and a kind declared in LeafRoute.h is actually in the table. Same shape as the
# general settings builder parity gate above, exemptions included — a route that genuinely belongs to one
# surface is declared with its reason, and a stale exemption fails.
#
# It reads comment-stripped text, so it cannot see an `#if 0` or a runtime `if`, and it only reads the three
# named files. Its first checks are that localLeafRoute is still DEFINED in the file it just read and that both
# dispatch functions were actually found — move any of them and the gate would otherwise go green while
# asserting nothing about a rule it claims to enforce.
echo "=== themed local-leaf routing parity ==="
LR_H="$HERE/../src/browse/LeafRoute.h"
LR_C="$HERE/../src/browse/LeafRoute.cpp"
LR_V="$HERE/../src/ui/HomeView.cpp"
lr_fail=0
lr_note() { echo "  $1"; lr_fail=1; }
# A LeafPlay route deliberately handled on ONE surface only, "Route|why". Empty today, and it should stay that
# way: a local kind that plays on one layout and not the other is the bug this gate exists for, not a
# configuration. Anything listed here is excused from the both-surfaces check below.
LR_SURFACE_ONLY=()
if [ ! -f "$LR_H" ] || [ ! -f "$LR_C" ] || [ ! -f "$LR_V" ]; then
  echo "FAIL: themed local-leaf routing parity (LeafRoute.{h,cpp} / HomeView.cpp not found under $HERE/../src)"; fail=1
else
  lr_h="$(mktemp)"; lr_c="$(mktemp)"; lr_v="$(mktemp)"
  lr_act="$(mktemp)"; lr_thm="$(mktemp)"; lr_tbl="$(mktemp)"
  sed -E 's://.*$::' "$LR_H" > "$lr_h"
  sed -E 's://.*$::' "$LR_C" > "$lr_c"
  sed -E 's://.*$::' "$LR_V" > "$lr_v"

  # --- The gate is reading what it thinks it is reading. ---
  grep -qE 'LeafRoute[[:space:]]+localLeafRoute[[:space:]]*\(' "$lr_c" \
    || lr_note "browse::localLeafRoute is not DEFINED in LeafRoute.cpp. The unit moved; every check below is vacuous, so treat a PASS as meaningless until this gate points at the new file."
  grep -qE 'localLeafKinds[[:space:]]*\(\)' "$lr_c" \
    || lr_note "the kinds table (localLeafKinds) is not in LeafRoute.cpp. Clause 3 below compares nothing."

  # Each dispatch function's body: from its signature to the first line that closes it at column 0. A file
  # operand is always supplied so awk can never fall back to the suite's own stdin.
  awk '/^void HomeView::activateItem\(/   { p = 1 } p { print } p && /^\}/ { exit }' "$lr_v" </dev/null > "$lr_act"
  awk '/^void HomeView::playThemedLeaf\(/ { p = 1 } p { print } p && /^\}/ { exit }' "$lr_v" </dev/null > "$lr_thm"
  lr_nact="$(wc -l < "$lr_act" | tr -d '[:space:]')"
  lr_nthm="$(wc -l < "$lr_thm" | tr -d '[:space:]')"
  # Floors well under today's sizes (~230 / ~110 lines). An empty or truncated region makes every clause below
  # vacuously true, which reads as "enforced" while enforcing nothing.
  [ "$lr_nact" -ge 60 ] || lr_note "HomeView::activateItem came out as $lr_nact line(s) — the signature this gate matches on has changed, or the function moved. It is not being checked."
  [ "$lr_nthm" -ge 30 ] || lr_note "HomeView::playThemedLeaf came out as $lr_nthm line(s) — the signature this gate matches on has changed, or the function moved. It is not being checked."

  # --- Clause 1: both surfaces consult the table. ---
  grep -q 'localLeafRoute' "$lr_act" \
    || lr_note "HomeView::activateItem does not call browse::localLeafRoute. The classic grid is deciding local routing on its own again — that is the second list this gate exists to prevent."
  grep -q 'localLeafRoute' "$lr_thm" \
    || lr_note "HomeView::playThemedLeaf does not call browse::localLeafRoute. The THEMED surface — the one this app is actually used through — is deciding local routing on its own again."

  # --- Clause 2: every route is handled on BOTH surfaces. ---
  # The enumerators, read out of the enum itself so a route ADDED to LeafPlay is checked without this gate
  # being edited. NotLocal is the "not ours" answer rather than a route, but it must still be NAMED on both
  # sides: a switch that silently defaults would hide the very drift being checked.
  lr_enums="$(awk '/enum class LeafPlay/ { p = 1; next } p && /\}/ { exit } p' "$lr_h" \
              | tr ',' '\n' | sed -E 's/[^A-Za-z]//g' | grep -E '^[A-Za-z]+$' | sort -u)"
  lr_nenum="$(printf '%s\n' "$lr_enums" | grep -c .)"
  [ "$lr_nenum" -ge 3 ] || lr_note "found $lr_nenum LeafPlay enumerator(s) — expected the whole enum. It was renamed or reshaped and this clause is comparing almost nothing."
  for lr_e in $lr_enums; do
    lr_excused=0
    for lr_x in ${LR_SURFACE_ONLY[@]+"${LR_SURFACE_ONLY[@]}"}; do
      [ "${lr_x%%|*}" = "$lr_e" ] && lr_excused=1
    done
    [ "$lr_excused" -eq 1 ] && continue
    grep -q "LeafPlay::$lr_e" "$lr_act" \
      || lr_note "route LeafPlay::$lr_e is not handled in HomeView::activateItem. A local kind that plays on the themed column and not in the classic grid — add the arm, or declare it in LR_SURFACE_ONLY with its reason."
    grep -q "LeafPlay::$lr_e" "$lr_thm" \
      || lr_note "route LeafPlay::$lr_e is not handled in HomeView::playThemedLeaf. This is EXACTLY the shipped bug: the row plays in the classic grid and answers \"Nothing to play\" on the themed XMB, which is the layout this app is used through. Add the arm, or declare it in LR_SURFACE_ONLY with its reason."
  done
  # Staleness: an exemption for a route that no longer exists reads exactly like a documented decision.
  for lr_x in ${LR_SURFACE_ONLY[@]+"${LR_SURFACE_ONLY[@]}"}; do
    printf '%s\n' "$lr_enums" | grep -qx -- "${lr_x%%|*}" \
      || lr_note "LR_SURFACE_ONLY names route \"${lr_x%%|*}\", which is not a LeafPlay enumerator. A stale exemption reads exactly like a documented decision — remove it."
    case "$lr_x" in *'|'*) ;; *) lr_note "LR_SURFACE_ONLY entry \"$lr_x\" carries no reason. The format is Route|why, and an unexplained exemption is indistinguishable from an oversight." ;; esac
  done

  # --- Clause 3: a declared kind is a routed kind. ---
  # Every constant in the marked block of LeafRoute.h must appear in LeafRoute.cpp's table. Declaring a
  # spelling and leaving it out of the table is a dead category — found by a user, rather than here.
  awk '/--- LOCAL LEAF KINDS/ { p = 1; next } /--- END LOCAL LEAF KINDS/ { exit } p' "$LR_H" \
    | grep -oE 'k[A-Za-z0-9_]+' | sort -u > "$lr_tbl"
  lr_nkind="$(grep -c . "$lr_tbl" | tr -d '[:space:]')"
  [ "$lr_nkind" -ge 3 ] || lr_note "found $lr_nkind local-leaf constant(s) in LeafRoute.h's marked block — expected the whole block. The markers moved; this clause is comparing almost nothing."
  while IFS= read -r lr_k; do
    [ -n "$lr_k" ] || continue
    grep -q -F -- "$lr_k" "$lr_c" \
      || lr_note "local-leaf kind $lr_k is declared in LeafRoute.h but never appears in LeafRoute.cpp. A catalog builder stamps rows with it and the table claims none of them — that whole category answers \"Nothing to play\" on both surfaces."
  done < "$lr_tbl"

  rm -f "$lr_h" "$lr_c" "$lr_v" "$lr_act" "$lr_thm" "$lr_tbl"
  if [ "$lr_fail" -eq 0 ]; then
    echo "PASS: themed local-leaf routing parity ($lr_nenum route(s), $lr_nkind declared kind(s), both dispatch sites on the table)"
  else
    echo "FAIL: themed local-leaf routing parity — the classic and themed Enter paths have drifted."; fail=1
  fi
fi
echo

# Synthetic-level Back survival gate. A synthetic level (Recent, ★ Favorites, Homebrew, a playlist, an OPDS
# feed) is pushed by an open…Level function and rebuilt by loadTop() after a Back. The two halves communicate
# through ONE channel: the marker the push stores in `lvl.item.mime`. Forget that one line and everything
# still compiles, the folder still opens, and the failure appears only when someone walks back out of an item
# — landing on an empty level, or on the level below. No probe can catch it: HomeView is 8k lines of widget
# and QML wiring that links nothing headlessly, which is exactly why this is a text gate.
#
# The property: every level type whose loadTop() arm REBUILDS ITSELF FROM `top.item.mime` must have a push
# site that ASSIGNS `item.mime`. The set is derived from loadTop() itself, so a level added later is covered
# without editing this gate; arms that rebuild from a store or a snapshot instead (`_musicroot`,
# `_traktcal`, the marks shelves) name no mime and are correctly not asked for one.
#
# Comments are stripped first, for the reason the gate above states at length: a gate whose prose names the
# symbols it greps for passes forever once the code is deleted and the prose is left behind.
echo "=== synthetic level Back survival ==="
SB_V="$HERE/../src/ui/HomeView.cpp"
sb_fail=0
sb_note() { echo "  $1"; sb_fail=1; }
if [ ! -f "$SB_V" ]; then
  echo "FAIL: synthetic level Back survival (HomeView.cpp not found under $HERE/../src)"; fail=1
else
  sb_v="$(mktemp)"; sb_lt="$(mktemp)"; sb_need="$(mktemp)"
  sed -E 's://.*$::' "$SB_V" > "$sb_v"
  awk '/^void HomeView::loadTop\(\)/ { p = 1 } p { print } p && /^\}/ { exit }' "$sb_v" </dev/null > "$sb_lt"
  sb_nlt="$(wc -l < "$sb_lt" | tr -d '[:space:]')"
  # A floor well under today's size (~113 lines). An empty region makes every clause below vacuously true,
  # which reads as "enforced" while enforcing nothing.
  [ "$sb_nlt" -ge 40 ] || sb_note "HomeView::loadTop came out as $sb_nlt line(s) — the signature this gate matches on has changed, or the function moved. It is not being checked."

  # Each arm runs from its own "if (top.detail …" to the next one; a marker read anywhere inside it means
  # that level rebuilds itself from its marker and therefore has to have stored one.
  sb_starts="$(grep -nE '^[[:space:]]*if \(top\.detail' "$sb_lt" | cut -d: -f1)"
  set -- $sb_starts
  while [ $# -gt 0 ]; do
    sb_s="$1"; shift
    sb_e="${1:-999999}"; sb_e=$((sb_e - 1))
    sb_ty="$(sed -n "${sb_s}p" "$sb_lt" | sed -nE 's/.*&& top\.item\.type == QStringLiteral\("(_[A-Za-z0-9]+)"\).*/\1/p')"
    [ -n "$sb_ty" ] || continue
    sed -n "${sb_s},${sb_e}p" "$sb_lt" | grep -q 'top\.item\.mime' && echo "$sb_ty" >> "$sb_need"
  done
  sb_nneed="$(sort -u "$sb_need" 2>/dev/null | grep -c . | tr -d '[:space:]')"
  [ "$sb_nneed" -ge 8 ] || sb_note "found $sb_nneed marker-rebuilt level type(s) in loadTop — expected the whole family. The arm shape this gate reads has changed and it is comparing almost nothing."

  while IFS= read -r sb_ty; do
    [ -n "$sb_ty" ] || continue
    sb_ln="$(grep -nE "\.item\.type = QStringLiteral\(\"$sb_ty\"\)" "$sb_v" | head -1 | cut -d: -f1)"
    if [ -z "$sb_ln" ]; then
      sb_note "loadTop rebuilds \"$sb_ty\" from its marker, but no push site assigns .item.type = \"$sb_ty\". Either the level is unreachable or this gate can no longer find where it is pushed."
      continue
    fi
    sb_from=$((sb_ln - 4)); [ "$sb_from" -lt 1 ] && sb_from=1
    sed -n "${sb_from},$((sb_ln + 8))p" "$sb_v" | grep -q '\.item\.mime = ' \
      || sb_note "the level pushed as \"$sb_ty\" never assigns .item.mime, but loadTop rebuilds that level FROM item.mime. Back out of anything opened from it and it repopulates from an empty marker — the folder opens, and only walking back out shows it is broken."
  done < <(sort -u "$sb_need" 2>/dev/null)

  rm -f "$sb_v" "$sb_lt" "$sb_need"
  if [ "$sb_fail" -eq 0 ]; then
    echo "PASS: synthetic level Back survival ($sb_nneed marker-rebuilt level type(s), each storing its marker)"
  else
    echo "FAIL: synthetic level Back survival — a synthetic level cannot be rebuilt after a Back."; fail=1
  fi
fi
echo

# Appearance theme-gallery reachability gate. openAppearance() has TWO builders — a themed one (PanelRows)
# and a classic one (QWidgets) — and CONTRIBUTING.md names that split as the thing most often half-done. The
# gallery is a fresh instance of it: the registry browser supported a Themes kind for a long time while being
# constructed with Addons at its ONE call site, so there was no theme gallery at all and nothing said so.
#
# This asserts the property rather than the prose: both builders still reach the registry. The themed side
# needs a row that OFFERS the gallery, a dispatch arm that HANDLES that row, and presentThemeRegistry both
# DEFINED and CALLED (a definition nothing calls is exactly the dead-code state this replaces); the classic
# side needs a RegistryBrowser::Themes construction.
#
# Comments are stripped FIRST, and that is load-bearing rather than tidiness: this whole section is
# introduced by prose naming every symbol it greps for, so a gate reading the raw file would go on passing
# after someone deleted the code and left a comment describing it. That is precisely how an assertion ends
# up gating nothing — the same trap the probe data-dir isolation gate documents.
#
# What the strip does NOT cover, said plainly so a PASS is not read for more than it earns: the preprocessor
# and control flow. Wrap either builder in `#if 0`, put the row behind a runtime `if`, build the browser in a
# lambda nothing connects — every token below is still there and this gate is still green. That is not a
# hypothetical shape: MainWindow.cpp already carries `void MainWindow::openAppearance() {}` in a `#else`
# branch. A grep cannot see any of it; only a UI-driving test could, and this suite is deliberately offline
# and windowless. Naming the hole beats a heuristic that would pretend to cover it.
echo "=== appearance theme-gallery reachability ==="
GAL_MW="$HERE/../src/ui/MainWindow.cpp"
if [ ! -f "$GAL_MW" ]; then
  echo "FAIL: appearance theme-gallery reachability (MainWindow.cpp not found at $GAL_MW)"
  fail=1
else
  gal_bad=0
  # Block comments come off first, then line comments. Neither order is a C lexer: taken this way round a
  # `//` comment holding an unbalanced `/*` opens a spurious block, and taken the other way a `/* … */`
  # holding a `//` loses its terminator. MainWindow.cpp has neither today — 77 `/* … */`, every one opened
  # and closed on its own line — and both failure modes OVER-strip, which can only produce a false FAIL.
  # That is the safe direction for a gate; under-stripping is the direction that lets a comment pass as code.
  gal_src="$(sed -E ':j; s@/\*([^*]|\*+[^*/])*\*+/@@g; /\/\*/ { $!{ N; bj } }' "$GAL_MW" | sed -E 's://.*$::')"

  # `grep -q` is deliberately NOT used on this stream, and that is not a style choice. This script runs under
  # `set -o pipefail` (top of file). grep -q exits the instant it matches, which SIGPIPEs the producer feeding
  # it; pipefail then reports the whole pipeline as rc=141 — a FAILURE — even though the pattern was found. On
  # the small files the other gates read the producer finishes inside the pipe buffer and it never shows, so
  # the idiom looks safe; MainWindow.cpp is ~760 KB, so it fires every single time. Written with grep -q this
  # gate failed all four assertions against a tree that satisfies all four, which is the mirror image of the
  # bug it exists to catch and would have been "fixed" by deleting the gate. `grep -c >/dev/null` drains the
  # stream, so the producer always completes; the exit status is still "did it match".
  #
  # -w (whole word) is the other thing the mutation pass paid for. A plain substring grep for
  # `RegistryBrowser::Themes` is satisfied by `RegistryBrowser::ThemesX`, so renaming the enumerator out from
  # under the classic builder left this gate green. Every pattern below is anchored on word boundaries now.
  #
  # GNU -w constrains BOTH ends regardless of what the pattern's own last character is: the man page's rule is
  # that the match must start at the line start or after a non-word character, and end at the line end or
  # before one. So on the two patterns closing with `)` it is doing work at the back too — the character after
  # that `)` must be a non-word character. Verified rather than assumed (GNU grep 3.0): with the pattern
  # `action(QStringLiteral("appr\.browse")`, a line ending `…"appr.browse")x;` does NOT match while one ending
  # `…"appr.browse");` does. That makes this gate STRICTER than a plain substring grep at both ends, not just
  # at the front, which is fine — every real call site is followed by `;` or `,` — but it is worth knowing:
  # a future pattern ending in a word character glued to a longer identifier would be rejected here.
  gal_has() { printf '%s\n' "$gal_src" | grep -cw "$1" >/dev/null; }

  # Floor: did this gate scan the right file at all? A gate that walks the wrong tree prints PASS, which is
  # worse than no gate — it reports a rule as enforced. openAppearance is the function both builders live in.
  if ! gal_has 'void MainWindow::openAppearance'; then
    echo "  MainWindow.cpp has no openAppearance definition — this gate scanned the wrong file or the"
    echo "  builders moved. Treat a PASS as meaningless until the path is fixed."
    gal_bad=1
  fi

  # Themed builder: the row that OFFERS the gallery, and the dispatch arm that HANDLES it. These are asserted
  # as two separate spellings rather than as the shared substring `"appr.browse"`, and that is the whole point:
  # the id appears twice — once building the PanelRow, once in onAct — so a single-occurrence check is
  # satisfied by either one alone. Deleting only the row leaves the dispatch arm holding both the id string
  # AND the presentThemeRegistry() call, so the row check and the call check below BOTH survive and the themed
  # surface loses its button with the suite fully green. Mutation-tested in each direction. Two spellings
  # rather than a >=2 occurrence count, which an unrelated third mention of the id would break.
  gal_has 'action(QStringLiteral("appr\.browse")' \
    || { echo "  the themed Appearance builder no longer offers an appr.browse row — the gallery has no"; \
         echo "  entry point on the themed surface"; gal_bad=1; }
  gal_has 'id == QStringLiteral("appr\.browse")' \
    || { echo "  nothing in the themed builder's onAct dispatches appr.browse — the row is offered but"; \
         echo "  pressing it does nothing"; gal_bad=1; }
  gal_has 'void MainWindow::presentThemeRegistry' \
    || { echo "  presentThemeRegistry is no longer defined — the themed gallery panel is gone"; gal_bad=1; }
  # Called, not merely defined. The definition line is excluded so it cannot satisfy its own call check. The
  # narrow grep runs FIRST so the second one is fed a couple of lines rather than the whole file — the same
  # SIGPIPE reasoning as above, and the reason this pipeline is safe to write as a pipeline.
  gal_calls="$(printf '%s\n' "$gal_src" | grep -n 'presentThemeRegistry()' \
                 | grep -v 'void MainWindow::presentThemeRegistry' || true)"
  [ -n "$gal_calls" ] \
    || { echo "  presentThemeRegistry is defined but never called — a themed panel nothing opens is the"; \
         echo "  exact dead-code state this gate exists to prevent"; gal_bad=1; }

  # Classic builder: the only way its gallery opens is a Themes-kind RegistryBrowser. Scope caveat, because
  # the PASS line must not claim more than this proves — the check is FILE-scoped, not scoped to the classic
  # builder's body. The two builders are branches of one function with no reliable textual boundary between
  # them, so what this actually asserts is "MainWindow.cpp constructs a Themes-kind browser somewhere". True
  # of the classic button today (one occurrence, MainWindow.cpp:5869), but a later refactor that built one
  # from the THEMED path would satisfy it after the classic one was gone. Hence the wording below.
  gal_has 'RegistryBrowser::Themes' \
    || { echo "  nothing in MainWindow.cpp constructs RegistryBrowser::Themes — the classic Appearance"; \
         echo "  builder's route into the gallery is gone"; gal_bad=1; }

  if [ "$gal_bad" -eq 0 ]; then
    echo "PASS: appearance theme-gallery reachability (every call site both builders need is present)"
  else
    echo "FAIL: appearance theme-gallery reachability — a user-facing surface exists on only one of"
    echo "  openAppearance()'s two builders. See the two-settings-builders rule in CONTRIBUTING.md."
    fail=1
  fi
fi
echo

# Themed host-property declaration gate. C++ pushes the whole themed scene's state in through
# QObject::setProperty() on the ThemeView root, and QML reads it back through bindings. When the name the host
# writes is not DECLARED in ThemeView.qml, that link is broken in a way NEITHER end reports:
#
#   * setProperty("notDeclared", v) does not fail. Qt attaches a DYNAMIC property to the QObject and returns
#     true. The host looks right, and the value genuinely is on the object — reading it straight back works.
#   * A dynamic property carries no change notification, so a QML binding that reads the name resolves it ONCE
#     (to undefined, before the host ever writes) and never re-evaluates. The element renders nothing, for ever.
#
# So the feature is simply dead, with no warning, no log line and no failing probe. It has shipped twice:
#   * `actionRomhack` — MainWindow set it to add a "Romhacks…" row to the XMB action chooser; ThemeView.qml
#     never declared it, so the chooser's row model read undefined once and the row could not appear on ANY
#     item, ever.
#   * `lyrics` / `lyricsSynced` / `lyricLine` — NowPlayingAudio.qml bound all three and ThemeView.qml declared
#     none, so the .lrc sidecar feature rendered nothing on any theme for its entire life while its parser
#     probe stayed green.
#
# Both directions are checked, because those two were found from opposite ends — the first from the setter, the
# second from the binding:
#   1. SETTER side: every setProperty("name", …) on a variable that holds the ThemeView root must name a
#      property ThemeView.qml declares at root level.
#   2. BINDING side: every `host.<name>` in an element (elements are handed the root as `item.host = root`)
#      must name a root-level property, function or signal of ThemeView.qml — or a QQuickItem builtin, which
#      is the short declared list below.
#
# Which variables hold the root is a TABLE, not a heuristic, and it is checked in both directions: every
# setProperty receiver in the scanned host files must be classified as root or not-root WITH ITS REASON, so a
# new pointer into the themed scene cannot arrive unclassified and be silently skipped. A stale entry fails —
# same discipline as the two parity gates above.
#
# What it CANNOT see, so a PASS is not read for more than it earns: it reads comment-stripped text from a
# fixed file list, so `#if 0`, a runtime `if`, a value smuggled inside a QVariantMap the QML digs into, or a
# host file not in TP_HOSTS are all invisible. It also cannot tell that a DECLARED property is actually bound
# by any element, or that a bound one is ever drawn — declaring `lyrics` and rendering nothing is still
# possible, and only driving the app finds that.
echo "=== themed host-property declarations ==="
TP_QML="$HERE/../src/theme2/qml/ThemeView.qml"
TP_ELE="$HERE/../src/theme2/qml/elements"
tp_fail=0
tp_note() { echo "  $1"; tp_fail=1; }
# The host files that reach the themed scene. Every setProperty RECEIVER in these is classified below.
TP_HOSTS=(
  "$HERE/../src/ui/MainWindow.cpp"
  "$HERE/../src/theme2/ThemeEngine.cpp"
  "$HERE/../src/theme2/ThemedPanelHost.cpp"
  "$HERE/../src/theme2/ThemePickerHost.cpp"
)
# basename:variable that IS the ThemeView root (each is a `ThemeEngine::rootItem(...)` result, or the
# ThemeBridge::root member that buildView assigns from qv->rootObject()). Properties written through these
# must be declared.
TP_ROOTS=(
  'MainWindow.cpp:r'
  'MainWindow.cpp:rr'
  'MainWindow.cpp:dr'
  'ThemeEngine.cpp:r'
  'ThemeEngine.cpp:root'
)
# basename:variable|why it is NOT the ThemeView root. These are skipped by the setter check.
TP_NOTROOT=(
  'ThemeEngine.cpp:qv|the QQuickWidget itself, not the QML root — it carries the mmvQuickView / mmvQuickRoot / mmvNavGraph markers that rootItem() and the nav kit look up, which are C++-side tags and deliberately not QML properties'
  'ThemedPanelHost.cpp:view_|the QQuickWidget hosting SettingsPanel.qml as its own root; the two names it sets are those same C++-side markers'
  'ThemePickerHost.cpp:view_|the QQuickWidget hosting ThemePicker.qml, a different root type; same C++-side markers'
)
# `host.<name>` uses that resolve on QQuickItem rather than on anything ThemeView declares. Kept short, and
# checked for staleness: an entry no element uses, or one ThemeView has since declared, fails.
TP_BUILTINS=( width height forceActiveFocus )

if [ ! -f "$TP_QML" ] || [ ! -d "$TP_ELE" ]; then
  echo "FAIL: themed host-property declarations (ThemeView.qml / elements not found under $HERE/../src/theme2/qml)"; fail=1
else
  tp_props="$(mktemp)"; tp_api="$(mktemp)"; tp_fns="$(mktemp)"
  tp_src="$(mktemp)"; tp_rcv="$(mktemp)"; tp_set="$(mktemp)"; tp_ref="$(mktemp)"

  # --- The ThemeView API, root level only. ---
  # The 4-space anchor is load-bearing rather than tidy: ThemeView.qml also declares properties INSIDE its
  # Repeater delegate and a few nested items (el, p, s, o, mg, fired, hit today). Those are not the root's
  # API, and accepting them would let an inner-item property satisfy a check about a host-set one.
  sed -E 's://.*$::' "$TP_QML" \
    | grep -oE '^ {4}(readonly[[:space:]]+)?property[[:space:]]+[A-Za-z_][A-Za-z0-9_<>]*[[:space:]]+[A-Za-z_][A-Za-z0-9_]*' \
    | sed -E 's/.*[[:space:]]//' | sort -u > "$tp_props"
  sed -E 's://.*$::' "$TP_QML" \
    | grep -oE '^ {4}(function|signal)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*' \
    | sed -E 's/.*[[:space:]]//' | sort -u > "$tp_fns"
  cat "$tp_props" "$tp_fns" | sort -u > "$tp_api"
  tp_nprop="$(grep -c . "$tp_props" | tr -d '[:space:]')"
  tp_nfn="$(grep -c . "$tp_fns" | tr -d '[:space:]')"
  # Corpus floors BEFORE any comparison. An empty API list would make BOTH checks below fail on everything —
  # loud, but a gate that cries wolf gets deleted rather than read. Floors are well under today's sizes
  # (65 root properties, 35 functions + signals).
  [ "$tp_nprop" -ge 50 ] || tp_note "found $tp_nprop root-level property declaration(s) in ThemeView.qml — expected the whole root API. The file was restructured or its root indentation changed; every check below is comparing against almost nothing."
  [ "$tp_nfn"   -ge 25 ] || tp_note "found $tp_nfn root-level function/signal declaration(s) in ThemeView.qml — expected the whole root API. Same restructure problem; the binding check below will report noise."

  # --- Setter side: what the host writes must be declared. ---
  tp_is_root()    { local e; for e in "${TP_ROOTS[@]}";   do [ "$e" = "$1" ] && return 0; done; return 1; }
  tp_is_notroot() { local e; for e in "${TP_NOTROOT[@]}"; do [ "${e%%|*}" = "$1" ] && return 0; done; return 1; }

  : > "$tp_set"
  for tp_h in "${TP_HOSTS[@]}"; do
    tp_base="$(basename "$tp_h")"
    if [ ! -f "$tp_h" ]; then
      tp_note "host file $tp_h is missing. It is in TP_HOSTS because it writes into the themed scene; if it moved, point this gate at the new path — until then nothing it does is checked."
      continue
    fi
    # Block comments off first, then line comments — the same order, and the same caveat, as the appearance
    # gate above: both failure modes of this pair OVER-strip, which can only produce a false FAIL, and that is
    # the safe direction for a gate. MainWindow.cpp carries prose about setProperty("items") that a raw grep
    # would read as code.
    sed -E ':j; s@/\*([^*]|\*+[^*/])*\*+/@@g; /\/\*/ { $!{ N; bj } }' "$tp_h" | sed -E 's://.*$::' > "$tp_src"

    # Every receiver, classified. This reverse direction is the point: without it, renaming `r` to `rt` in one
    # function would drop that function's writes out of the gate's view with everything still green.
    grep -oE '[A-Za-z_][A-Za-z0-9_]*->setProperty\(' "$tp_src" | sed 's/->setProperty(//' | sort -u > "$tp_rcv"
    while IFS= read -r tp_v; do
      [ -n "$tp_v" ] || continue
      if tp_is_root "$tp_base:$tp_v"; then
        # `grep -o` on a 760 KB file is fine here and `grep -q` would not be: this script runs under
        # `set -o pipefail`, and -q exits on first match, SIGPIPEs the producer and reports rc=141 for a
        # pipeline that MATCHED. The consumers below drain the stream, so the producer always completes.
        grep -oE "\\b$tp_v->setProperty\\(\"[A-Za-z_][A-Za-z0-9_]*\"" "$tp_src" \
          | sed -E 's/.*\("//; s/"$//' | sort -u >> "$tp_set"
      elif tp_is_notroot "$tp_base:$tp_v"; then
        :
      else
        tp_note "\`$tp_v->setProperty(...)\` in $tp_base is not classified by this gate. Say whether $tp_v holds the ThemeView root: add \"$tp_base:$tp_v\" to TP_ROOTS if it does, or to TP_NOTROOT with its reason if it does not. An unclassified receiver is silently unchecked, which is how the bug this gate exists for gets back in."
      fi
    done < "$tp_rcv"
  done
  sort -u "$tp_set" -o "$tp_set"
  tp_nset="$(grep -c . "$tp_set" | tr -d '[:space:]')"
  [ "$tp_nset" -ge 30 ] || tp_note "collected $tp_nset host-set propert(y/ies) — expected the full themed scene state (39 today). The receivers moved or TP_HOSTS is wrong, and this gate is checking almost nothing."

  while IFS= read -r tp_n; do
    [ -n "$tp_n" ] || continue
    grep -qx -- "$tp_n" "$tp_props" \
      || tp_note "the host sets \"$tp_n\" on the ThemeView root, and native/src/theme2/qml/ThemeView.qml DECLARES NO SUCH PROPERTY. Nothing fails at runtime and nothing appears in any log: setProperty() attaches a dynamic property and returns true, and a QML binding reading a dynamic property resolves it once as undefined and never re-evaluates, because dynamic properties carry no change notification. The feature is dead on every theme. Declare \`property <type> $tp_n\` at ROOT level in ThemeView.qml (4-space indent, with a comment naming who writes it) — that is the whole fix, and it is all either of the two shipped regressions this gate is written from needed."
  done < "$tp_set"

  # --- Binding side: what an element reads off `host` must exist. ---
  # Elements are handed the root by ThemeView's own loader (`item.host = root`), so `host.<name>` is a read of
  # the root's API. This is the end the lyrics bug was found from: NowPlayingAudio.qml bound three names the
  # root had never heard of, and nothing said so.
  for tp_f in "$TP_ELE"/*.qml; do
    [ -f "$tp_f" ] || continue
    sed -E ':j; s@/\*([^*]|\*+[^*/])*\*+/@@g; /\/\*/ { $!{ N; bj } }' "$tp_f" | sed -E 's://.*$::'
  done | grep -oE '\bhost\.[A-Za-z_][A-Za-z0-9_]*' | sed 's/^host\.//' | sort -u > "$tp_ref"
  tp_nref="$(grep -c . "$tp_ref" | tr -d '[:space:]')"
  [ "$tp_nref" -ge 30 ] || tp_note "found $tp_nref \`host.<name>\` reference(s) across $TP_ELE — expected the whole element set (42 today). The elements moved, or stopped reaching the root through \`host\`; this half of the gate is checking almost nothing."

  tp_is_builtin() { local e; for e in "${TP_BUILTINS[@]}"; do [ "$e" = "$1" ] && return 0; done; return 1; }
  while IFS= read -r tp_n; do
    [ -n "$tp_n" ] || continue
    grep -qx -- "$tp_n" "$tp_api" && continue
    tp_is_builtin "$tp_n" && continue
    tp_who="$(grep -rlF "host.$tp_n" "$TP_ELE" | sed 's@.*/@@' | sort | tr '\n' ' ' | sed 's/ *$//')"
    tp_note "an element binds \`host.$tp_n\` (in $tp_who), and ThemeView.qml declares no such property, function or signal. \`host\` IS the ThemeView root (\`item.host = root\`), so that binding resolves to undefined and stays undefined — silently, with no warning at either end, even when the host later writes the name. Declare it at root level in ThemeView.qml, or fix the element's spelling. This is the end the .lrc lyrics break was found from: three names bound, none declared, nothing rendered on any theme."
  done < "$tp_ref"

  # --- Staleness. An entry that classifies nothing reads exactly like a considered decision. ---
  for tp_e in "${TP_NOTROOT[@]}"; do
    case "$tp_e" in *'|'*) ;; *) tp_note "TP_NOTROOT entry \"$tp_e\" carries no reason. The format is basename:var|why, and an unexplained exemption is indistinguishable from an oversight." ;; esac
  done
  # Every classified receiver must still exist, in the file it names. An entry for a variable that is gone
  # stops asserting anything while still reading as documented.
  for tp_e in "${TP_ROOTS[@]}" "${TP_NOTROOT[@]}"; do
    tp_k="${tp_e%%|*}"; tp_b="${tp_k%%:*}"; tp_v="${tp_k##*:}"; tp_hit=0
    for tp_h in "${TP_HOSTS[@]}"; do
      [ "$(basename "$tp_h")" = "$tp_b" ] || continue
      [ -f "$tp_h" ] || continue
      [ "$(grep -cE "\\b$tp_v->setProperty\\(" "$tp_h")" -gt 0 ] && tp_hit=1
    done
    [ "$tp_hit" -eq 1 ] \
      || tp_note "this gate classifies \"$tp_k\", but $tp_b has no \`$tp_v->setProperty(\` at all. Either the variable was renamed (in which case its writes are now UNCLASSIFIED, and the check above will have said so) or the code is gone — remove the entry."
  done
  for tp_e in "${TP_BUILTINS[@]}"; do
    [ "$(grep -c -x -- "$tp_e" "$tp_ref")" -gt 0 ] \
      || tp_note "TP_BUILTINS allows \`host.$tp_e\`, which no element reads any more. A stale allowance reads exactly like a documented decision — remove it."
    grep -qx -- "$tp_e" "$tp_api" \
      && tp_note "\`$tp_e\` is BOTH allowed as a QQuickItem builtin and declared by ThemeView.qml. The declaration shadows the builtin, so the allowance is now hiding a real check — drop it from TP_BUILTINS."
  done

  rm -f "$tp_props" "$tp_api" "$tp_fns" "$tp_src" "$tp_rcv" "$tp_set" "$tp_ref"
  if [ "$tp_fail" -eq 0 ]; then
    echo "PASS: themed host-property declarations ($tp_nset host-set, $tp_nref bound via host., against $tp_nprop declared propert(y/ies) + $tp_nfn function(s)/signal(s))"
  else
    echo "FAIL: themed host-property declarations — the host and ThemeView.qml disagree about what the themed"
    echo "  root has. See the host-set-property rule in CONTRIBUTING.md; the symptom in the app is a feature"
    echo "  that renders nothing at all, on every theme, with no error anywhere."
    fail=1
  fi
fi
echo

# RetroPark integration spike — the driven-core green-pixel proof. RetroPark is now a PERMANENT dependency (a git
# submodule, built unconditionally on desktop/Windows in native/CMakeLists.txt), so probe_retropark exists whenever
# the tree was built on Windows. It links Vulkan + D3D11, so it is a Windows-only target: on a Linux/CI-Linux build
# the binary does not exist. That is why it is findexe-guarded here rather than in the mandatory `for p in` list
# above (which treats a missing binary as a FAIL) — a Linux run must still reach ALL HEADLESS PROBES PASSED without
# it. When built, it must pass: it creates a headless RetroPark runtime (D3D11 + null window), loads a driven
# reference core via the DLL-free static-core path, and asserts a real green frame read back inside this process
# (RETROPARK-OK — also emitted on a graceful no-D3D11-device skip).
#
# CI runs it: the `retropark-windows` job in .github/workflows/ci.yml builds the submodule on a Windows runner and
# executes this probe there (windows-2022 has a WARP D3D11 device), so the Windows-only coverage is not lost.
if RETROPARK="$(findexe probe_retropark)"; then
  run "retropark driven-core" RETROPARK-OK "$RETROPARK"
else
  echo "(skip) probe_retropark not built — RetroPark is a desktop/Windows-only dependency (run on Windows, or in the retropark-windows CI job)"; echo
fi

# RetroPark live-loop proof (Slice 2a) — the behavioural guard behind RetroParkView's play surface: present
# ADVANCES the driven core, pause FREEZES it (present repeats the retained frame byte-for-byte), resume advances
# again. Same optionality as probe_retropark: it is a Windows-only target (RetroPark links Vulkan + D3D11, guarded
# to desktop/Windows in native/CMakeLists.txt), so on a Linux/CI build the binary does not exist and this is a
# skip — hence the findexe guard, NOT the mandatory `for p in` list above. When built it must pass (RETROPARK-LOOP-OK,
# also emitted on a graceful no-D3D11-device skip).
if RETROPARK_LOOP="$(findexe probe_retropark_loop)"; then
  run "retropark live-loop" RETROPARK-LOOP-OK "$RETROPARK_LOOP"
else
  echo "(skip) probe_retropark_loop not built — RetroPark is a desktop/Windows-only dependency"; echo
fi

# RetroPark real-content proof (Slice 2b) — the guard behind RetroParkView's DYNAMIC shim path. Asserts the
# runtime's manifest parser accepts the shim's core.json (abi_version:4 vs header 5) and, when the shim + fceumm
# are staged beside the exe and a D3D11 device exists, that rp_runtime_load_core loads the shim and
# rp_runtime_load_content rejects a bogus ROM. Same Windows-only optionality as the two probes above (findexe
# guard). When built it must pass (RETROPARK-CONTENT-OK — also printed on a graceful no-device SKIPPED or a
# no-fceumm DEFERRED, so the gate stays green where the full runtime load cannot run).
if RETROPARK_CONTENT="$(findexe probe_retropark_content)"; then
  run "retropark real-content" RETROPARK-CONTENT-OK "$RETROPARK_CONTENT"
else
  echo "(skip) probe_retropark_content not built — RetroPark is a desktop/Windows-only dependency"; echo
fi

# RetroPark PRESENTING-core spike (Slice 3a) — the heavy-app path de-risk. Creates a HEADLESS VULKAN runtime
# (RP_GFX_VULKAN + null window — the only config rp_runtime_present reads a presenting frame back on) and loads
# the DYNAMIC refcore_present_vk core (staged beside the exe by the build), then asserts the presenting core's
# Vulkan-rendered frame reads back inside this process (its green lands in the bottom-right quadrant). Same
# Windows-only optionality as the driven probes above (findexe guard): RetroPark links Vulkan + D3D11 and is
# desktop/Windows-guarded, so on a Linux/CI build the binary does not exist and this is a skip. When built it
# must pass (RETROPARK-PRESENT-OK — also emitted on a graceful no-Vulkan-device SKIPPED so a GPU-less gate stays
# green). CI runs it on the retropark-windows job (which has a Vulkan device), so the real render is exercised.
if RETROPARK_PRESENT="$(findexe probe_retropark_present)"; then
  run "retropark presenting-core" RETROPARK-PRESENT-OK "$RETROPARK_PRESENT"
else
  echo "(skip) probe_retropark_present not built — RetroPark is a desktop/Windows-only dependency"; echo
fi

# RetroPark DOLPHIN presenting-core load (Slice 3b) — the specific-core proof for RetroParkView's GC path: create a
# Vulkan runtime + rp_runtime_load_core the local Dolphin vehicle (no load_content). Emits RETROPARK-DOLPHIN-OK on
# the real load proof AND on a graceful skip (no Vulkan device, OR the local-only vehicle not staged — CI / fresh
# clone), so a GPU-less or vehicle-less gate stays green. The full GC boot is the live Task-6 proof.
if RETROPARK_DOLPHIN="$(findexe probe_retropark_dolphin)"; then
  run "retropark dolphin-core" RETROPARK-DOLPHIN-OK "$RETROPARK_DOLPHIN"
else
  echo "(skip) probe_retropark_dolphin not built — RetroPark is a desktop/Windows-only dependency"; echo
fi

# RetroPark runtime-API selector (Slice 3a) — the pure unit test behind RetroParkView's create-before-load-core
# decision: a presenting core maps to a Vulkan runtime, a driven core to D3D11 (rpapi::runtimeApiForCore). Header
# only (no GPU, no link), but Windows-only-BUILT because the RetroPark submodule headers live under the desktop
# guard, so it is findexe-guarded like the others. When built it must pass (RETROPARK-APISELECT-OK).
if RETROPARK_APISELECT="$(findexe probe_retropark_apiselect)"; then
  run "retropark api-select" RETROPARK-APISELECT-OK "$RETROPARK_APISELECT"
else
  echo "(skip) probe_retropark_apiselect not built — RetroPark is a desktop/Windows-only dependency"; echo
fi

# The audio leaf refuses a payload that cannot be audio (issue #207). The DECISION is pure and
# probe_resolver pins every clause of it (CatalogMatch::payloadShape); probe_docbridge pins the picker that
# keeps an ebook release out of an audiobook search in the first place. What neither can reach is
# MainWindow::openLibraryItem, which no probe links - and the user-visible half of #207 lives exactly there:
# a resolved zip endpoint was handed to the player, which came up and did nothing, and said nothing.
#
# Deleting the six lines that ask the question puts the app back to a silent dead player with the whole
# suite green, which is the shape of defect this gate family exists for. Three properties are checked: the
# question is asked, it is asked BEFORE anything stages playback, and a bad answer ENDS the open (a notice
# and a return) rather than falling through.
echo "=== audio leaf refuses a non-audio payload ==="
AP_MW="$HERE/../src/ui/MainWindow.cpp"
ap_fail=0
ap_note() { echo "  $1"; ap_fail=1; }
if [ ! -f "$AP_MW" ]; then
  echo "FAIL: audio leaf refuses a non-audio payload (MainWindow.cpp not found)"; fail=1
else
  # Comments stripped, and matched with `grep -c -F` against a FILE: this source is far too big for
  # `printf … | grep -q`, which SIGPIPEs the printf under `set -o pipefail` and reports failure ON A MATCH.
  ap_tmp="$(mktemp)"
  sed -E 's://.*$::' "$AP_MW" > "$ap_tmp"
  [ "$(wc -l < "$ap_tmp")" -gt 100 ] || ap_note "the stripped MainWindow.cpp corpus is empty — this gate scanned nothing."

  ap_n="$(grep -c -F 'CatalogMatch::payloadShape(url, item.mime)' "$ap_tmp")"
  [ "$ap_n" -eq 1 ] || ap_note "MainWindow::openLibraryItem asks CatalogMatch::payloadShape(url, item.mime) $ap_n time(s) (expected 1): without it an audiobook that resolved to a zip or an ebook is staged as playback and the player opens on nothing."
  for ap_s in Document Archive; do
    [ "$(grep -c -F "CatalogMatch::PayloadShape::$ap_s" "$ap_tmp")" -gt 0 ] \
      || ap_note "the refusal does not name CatalogMatch::PayloadShape::$ap_s — a payload of that shape would be played anyway."
  done

  # Order, by line number. The guard is worthless below the leaf it guards, and the split-screen block plays
  # the same nothing in a pane that the full-screen player does — so it must precede BOTH.
  ap_guard="$(grep -n -F 'CatalogMatch::payloadShape(url, item.mime)' "$ap_tmp" | head -1 | cut -d: -f1)"
  ap_play="$(grep -n -F 'noteStreamScrobble(item, type);' "$ap_tmp" | head -1 | cut -d: -f1)"
  # The split block is located by the one line only IT has. Its hand-off to the pane
  # (splitTarget_->openVideo(...)) is NOT unique — the IPTV branch far above uses the same call, and a gate
  # anchored on that compares the guard against the wrong line entirely (this gate's first draft did, and
  # said so loudly, which is the only reason the anchor is spelled out here).
  ap_pane="$(grep -n -F 'const bool isGame = (type == QStringLiteral("game")' "$ap_tmp" | head -1 | cut -d: -f1)"
  if [ -z "$ap_guard" ] || [ -z "$ap_play" ] || [ -z "$ap_pane" ]; then
    ap_note "could not locate the guard, the audio leaf and the split-screen hand-off to compare their order (guard=$ap_guard leaf=$ap_play pane=$ap_pane)."
  else
    [ "$ap_guard" -lt "$ap_play" ] || ap_note "the payload-shape guard (line $ap_guard) sits BELOW the audio leaf (line $ap_play): the player is already staged by the time the question is asked."
    [ "$ap_guard" -lt "$ap_pane" ] || ap_note "the payload-shape guard (line $ap_guard) sits BELOW the split-screen hand-off (line $ap_pane): a pane would play the same nothing."
  fi

  # A refusal ends the open and SAYS so. Checked over the guard's own block rather than the whole file,
  # because notify() and return; are everywhere in this function.
  if [ -n "$ap_guard" ]; then
    ap_block="$(sed -n "${ap_guard},$((ap_guard + 20))p" "$ap_tmp")"
    ap_btmp="$(mktemp)"; printf '%s\n' "$ap_block" > "$ap_btmp"
    [ "$(grep -c -F 'notify(' "$ap_btmp")" -gt 0 ] \
      || ap_note "the refusal does not notify() — a silent refusal teaches the user the app is broken just as effectively as a dead player did."
    [ "$(grep -c -F 'return;' "$ap_btmp")" -gt 0 ] \
      || ap_note "the refusal does not return — it would fall through to the player it exists to prevent."
  fi

  # The doc-bridge half, in the one place that knows which shelf was searched. A picker that stops asking is
  # the original bug: an EPUB release winning an audiobook search on a title match alone.
  AP_AM="$HERE/../src/addons/AddonManager.cpp"
  if [ ! -f "$AP_AM" ]; then
    ap_note "AddonManager.cpp not found — the doc-bridge format gate could not be checked."
  else
    ap_atmp="$(mktemp)"
    sed -E 's://.*$::' "$AP_AM" > "$ap_atmp"
    [ "$(grep -c -F 'CatalogMatch::formatMatchesRequest(wantFormat, wantTitle, it.title)' "$ap_atmp")" -eq 1 ] \
      || ap_note "the doc-bridge picker does not ask CatalogMatch::formatMatchesRequest exactly once: an ebook release can win an audiobook search on its title alone (#207)."
    [ "$(grep -c -F 'CatalogMatch::catalogWantsFormat(catalogType)' "$ap_atmp")" -eq 1 ] \
      || ap_note "the picker does not derive the wanted format from the CATALOG it searched — the candidate does not get a vote on what was asked for."
  fi
fi
if [ "$ap_fail" -eq 0 ]; then echo "PASS: audio leaf refuses a non-audio payload"; else echo "FAIL: audio leaf refuses a non-audio payload"; fail=1; fi
echo

# Exe-folder contamination gate (issue #42). The suite's own answer to "did any probe touch the app's data
# directory". Every probe binary sits next to the GUI exe, and on desktop that folder IS the app's data dir —
# so before the isolation went in, a suite run left an everythingbox.ini (carrying one-shot add-on migration
# flags, no less), addons/, themes/, metadata/ and stream_debug.log behind for whatever ran there next. This
# compares the folder's app-data footprint across the whole run: nothing may appear, change or vanish.
echo "=== exe-folder contamination ==="
EXE_FP_AFTER="$(exe_fingerprint)"
if [ "$EXE_FP_BEFORE" = "$EXE_FP_AFTER" ]; then
  echo "PASS: exe-folder contamination ($EXE_DIR unchanged across the suite)"
else
  echo "$EXE_DIR changed while the suite ran:"
  diff <(printf '%s\n' "$EXE_FP_BEFORE") <(printf '%s\n' "$EXE_FP_AFTER") | sed 's/^/  /'
  # Deliberately not "a probe did it". This gate watches a directory, not a process, and it cannot tell a
  # probe apart from anything else that wrote there while it ran: a GUI launched from build/Release, or a
  # concurrent session building a probe exe into it — this working tree is shared between sessions. Naming
  # the wrong culprit confidently is the exact failure mode (a gate that cries wolf) this whole issue is
  # about, so the message names the alternatives and lets the diff above decide.
  echo "FAIL: exe-folder contamination — something changed the folder the app's data lives in during the run."
  echo "  Most likely a probe wrote there. It can also be an app or a build running out of that folder at the"
  echo "  same time (a GUI launched from it, or a concurrent build dropping a new exe in) — the diff says which."
  fail=1
fi
echo

# No verdict is printed here any more (issue #180). This process produces EVIDENCE - PASS:/FAIL: lines and an
# exit status - and the parent that ran it through `tee` derives the verdict from that transcript. That is what
# makes the summary line underivable from anything except the per-probe lines, and what lets a probe that dies
# before printing `FAIL:` still be named.
exit "$fail"
