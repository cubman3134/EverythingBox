#!/usr/bin/env bash
# Headless probe suite — the automated tests CI runs on every push/PR. These need no display, no GPU, and no
# ROMs: they spin up real subsystem code and assert a success sentinel.
#
#   * netplay relay        — two NetplaySession instances (two "emulators") pair through the relay, sync a save
#                            state, and exchange an input packet (probe_netplay -> NETPLAY-RELAY-OK).
#   * netplay both:direct  — the "Both" online orchestration with the relay dead, so ONLY a direct connection can
#                            pair them (probe_netplay_both direct -> NETPLAY-BOTH-OK).
#   * netplay both:relay   — same, but the direct endpoint is dead so it must fall back to the relay.
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
RELAY_PORT="${RELAY_PORT:-55677}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELAY_PY="$HERE/netplay-relay.py"
PY="${PYTHON:-python3}"; command -v "$PY" >/dev/null 2>&1 || PY=python

# The suite owns the probes' data-dir configuration; the two hand-run escape hatches must not survive into it
# (issue #42). EB_PROBE_DATA_DIR pins every probe at ONE directory that AppPaths never cleans up (owned=false),
# so a single forgotten `export` in a developer's profile silently restores the exact collision this whole
# scheme exists to kill — state accreting across probes AND across runs — and nothing notices: probe_isolation
# scrubs the pin from its own child's environment and compares against the exe folder, so it passes anyway.
# EB_PROBE_DATA_DIR_KEEP is the same shape one step down: it disables cleanup, and the suite would leave a
# directory per probe per run behind. Both stay fully usable for running a probe by hand — this only says the
# suite starts from a known state.
unset EB_PROBE_DATA_DIR EB_PROBE_DATA_DIR_KEEP

# A probe exe may land at build/<name>, build/<name>.exe, or build/Release/<name>[.exe] (multi-config generators).
findexe() {
  local n="$1" p
  for p in "$BUILD_DIR/$n" "$BUILD_DIR/$n.exe" "$BUILD_DIR/Release/$n" "$BUILD_DIR/Release/$n.exe"; do
    [ -x "$p" ] && { echo "$p"; return 0; }
  done
  return 1
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
run() { # <name> <sentinel> <exe> [args...]
  local name="$1" sentinel="$2"; shift 2
  echo "=== $name ==="
  local out rc
  out="$("$@" 2>&1)"; rc=$?
  echo "$out"
  if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q "$sentinel"; then
    echo "PASS: $name"
  else
    echo "FAIL: $name (rc=$rc, expected '$sentinel')"; fail=1
  fi
  echo
}

# Bring up the relay both netplay tests rendezvous through.
"$PY" "$RELAY_PY" --port "$RELAY_PORT" > /tmp/eb-relay.log 2>&1 &
RELAY_PID=$!
trap 'rm -rf "$EB_PROBE_SCRATCH_ROOT_POSIX"; [ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null' EXIT
for _ in $(seq 1 40); do grep -q "listening" /tmp/eb-relay.log 2>/dev/null && break; sleep 0.2; done
echo "relay: $(cat /tmp/eb-relay.log 2>/dev/null | head -1)"; echo

NETPLAY="$(findexe probe_netplay)"       || { echo "FATAL: probe_netplay not built"; exit 2; }
BOTH="$(findexe probe_netplay_both)"     || { echo "FATAL: probe_netplay_both not built"; exit 2; }
NAV="$(findexe probe_nav)"               || { echo "FATAL: probe_nav not built"; exit 2; }
META="$(findexe probe_meta)"             || { echo "FATAL: probe_meta not built"; exit 2; }
ISO="$(findexe probe_isolation)"         || { echo "FATAL: probe_isolation not built"; exit 2; }

# The folder the probe binaries were built into — which on desktop is also what AppPaths::dataDir() used to
# resolve to, i.e. the app's whole data directory. Everything below that talks about "the exe folder" means
# this one.
EXE_DIR="$(cd "$(dirname "$ISO")" && pwd)"

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
for p in "probe_navqml NAVQML-OK" "probe_themeview THEMEVIEW-OK" "probe_notifier NOTIFIER-OK" "probe_m3u M3U-OK" "probe_playback PLAYBACK-OK" "probe_browse BROWSE-OK" "probe_perf PERF-OK" "probe_formfactor FORMFACTOR-OK" "probe_bootstrap BOOTSTRAP-OK" "probe_sync SYNC-OK" "probe_extplayer EXTPLAYER-OK" "probe_marks MARKS-OK" "probe_stats STATS-OK" "probe_playlists PLAYLISTS-OK" "probe_cloudmerge CLOUDMERGE-OK" "probe_importers IMPORTERS-OK" "probe_onboarding ONBOARDING-OK" "probe_locallib LOCALLIB-OK" "probe_resolver RESOLVER-OK" "probe_showdispatch SHOWDISPATCH-OK" "probe_subs SUBS-OK" "probe_segments SEGMENTS-OK" "probe_stremio STREMIO-OK" "probe_savesync SAVESYNC-OK" "probe_brand BRAND-OK" "probe_theme THEME-OK" "probe_settingstxn SETTINGSTXN-OK" "probe_trakt TRAKT-OK" "probe_passcode PASSCODE-OK" "probe_pcgames PCGAMES-OK" "probe_crashreport CRASHREPORT-OK"; do
  set -- $p
  if exe=$(findexe "$1"); then run "$1" "$2" "$exe"; else echo "(skip) $1 not built"; fi
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

  for cr_fn in writeFirstChanceRecord writeFatalRecord; do
    printf '%s\n' "$cr_src" | grep -q "__declspec(noinline) void $cr_fn(" \
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
  printf '%s\n' "$ms_src" | grep -q 'scrapedDetail_\.remember(' \
    || ms_note "nothing stamps the snapshot any more (showMeta's fromProvider branch): the editor drops back to the cache for every item, and the open card visibly strips on each edit."
  ms_body="$(printf '%s\n' "$ms_src" | awk '
    /^MediaDetail HomeView::detailScrapedValues\(\) const/ { inbody = 1 }
    inbody { print }
    inbody && /^}/ { exit }')"
  if [ -z "$(printf '%s' "$ms_body" | tr -d '[:space:]')" ]; then
    ms_note "HomeView::detailScrapedValues not found — the gate stopped matching its signature."
  else
    printf '%s' "$ms_body" | grep -q 'scrapedDetail_\.forKey(' \
      || ms_note "detailScrapedValues does not read the snapshot through forKey()."
    printf '%s' "$ms_body" | grep -q 'MetaCache::keyFor(' \
      || ms_note "detailScrapedValues no longer derives the open item's key — forKey() is only as honest as the key handed to it."
  fi
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
#   * main.cpp                        — only migrateLegacySettings() and its comment block, carved out by the
#                                       awk range below rather than excluding the whole 1000-line file: that
#                                       function is the Goliath->MyMediaVault hop, where the old name is the
#                                       subject matter and retargeting it at the current ini is a data-loss bug.
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
       ':(exclude)native/src/main.cpp' \
       ':(exclude)native/addon-protocol/aiocatalog-worker/src/worker.js' \
       ':(exclude)native/addon-protocol/aiocatalog-worker/wrangler.toml' \
       ':(exclude)native/addon-protocol/aiocatalog-worker/README.md' \
       ':(exclude)native/tools/probe_playlists.cpp' \
       ':(exclude)native/secrets/README.md' \
       ':(exclude)native/resources/Uninstall.cmd' \
       ':(exclude)docs/superpowers/specs/2026-07-27-everythingbox-rebrand-design.md' \
       ':(exclude)docs/superpowers/plans/2026-07-27-everythingbox-rebrand-plan.md' || true)"
# main.cpp is gated on everything OUTSIDE migrateLegacySettings — its exempt region runs from the comment
# block that opens with the Goliath naming note down to that function's closing brace.
MAINCPP="$HERE/../src/main.cpp"
if [ -f "$MAINCPP" ]; then
  main_hits="$(awk '
    /^\/\/ One-time migration from the ORIGINAL "Goliath" naming/ { inmig = 1 }
    inmig && /^}/ { inmig = 0; next }
    !inmig { print FILENAME":"NR": "$0 }
  ' "$MAINCPP" | grep -i -E 'mymediavault|my media vault|\<MMV\>|MMV_' || true)"
  [ -n "$main_hits" ] && brand_hits="$brand_hits"$'\n'"$main_hits"
else
  echo "FAIL: old-brand references (main.cpp not found at $MAINCPP)"; fail=1
fi
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
  # The auto-apply loop must still be there, still keyed on the probe_ name prefix. Without it a new probe is
  # silently un-isolated, which is the trap this issue was about rather than a fix for it.
  printf '%s\n' "$iso_src" | grep -q 'BUILDSYSTEM_TARGETS' \
    || { echo "  the BUILDSYSTEM_TARGETS sweep is gone — isolation is no longer applied to every probe"; iso_bad=1; }
  printf '%s\n' "$iso_src" | grep -q '_eb_t MATCHES "\^probe_"' \
    || { echo "  the ^probe_ name match is gone — a new probe no longer gets isolation by default"; iso_bad=1; }
  printf '%s\n' "$iso_src" | grep -q 'target_compile_definitions(\${_eb_t} PRIVATE EB_ISOLATED_DATA_DIR)' \
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
# checked-in record of what was last synced there, native/themes2/REGISTRY-SYNC.json. Edit a bundled theme
# and its canonical hash moves; this goes red and names the theme, the file that has to be republished, and
# the command that refreshes the record. Be clear about what that buys: the record states an INTENT, and
# --update can be run by someone who never pushed. It converts a SILENT drift into a deliberate one — the
# person editing the theme is told, at the moment of the edit, that a second copy exists. The guarantee
# needs a publish job (a workflow that pushes the changed themes to the registry itself), which needs a
# cross-repo write credential; REGISTRY-SYNC.json spells that out.
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
  echo "FAIL: bundled-theme / registry drift — a bundled theme has moved away from the copy the community"
  echo "  registry serves under the same name. Republish it and rerun with --update (details above)."
  fail=1
fi
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

if [ "$fail" -eq 0 ]; then echo "ALL HEADLESS PROBES PASSED"; else echo "SOME HEADLESS PROBES FAILED"; fi
exit "$fail"
