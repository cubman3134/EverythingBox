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

# A probe exe may land at build/<name>, build/<name>.exe, or build/Release/<name>[.exe] (multi-config generators).
findexe() {
  local n="$1" p
  for p in "$BUILD_DIR/$n" "$BUILD_DIR/$n.exe" "$BUILD_DIR/Release/$n" "$BUILD_DIR/Release/$n.exe"; do
    [ -x "$p" ] && { echo "$p"; return 0; }
  done
  return 1
}

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
trap '[ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null' EXIT
for _ in $(seq 1 40); do grep -q "listening" /tmp/eb-relay.log 2>/dev/null && break; sleep 0.2; done
echo "relay: $(cat /tmp/eb-relay.log 2>/dev/null | head -1)"; echo

NETPLAY="$(findexe probe_netplay)"       || { echo "FATAL: probe_netplay not built"; exit 2; }
BOTH="$(findexe probe_netplay_both)"     || { echo "FATAL: probe_netplay_both not built"; exit 2; }
NAV="$(findexe probe_nav)"               || { echo "FATAL: probe_nav not built"; exit 2; }
META="$(findexe probe_meta)"             || { echo "FATAL: probe_meta not built"; exit 2; }

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
for p in "probe_navqml NAVQML-OK" "probe_notifier NOTIFIER-OK" "probe_m3u M3U-OK" "probe_playback PLAYBACK-OK" "probe_browse BROWSE-OK" "probe_perf PERF-OK" "probe_formfactor FORMFACTOR-OK" "probe_bootstrap BOOTSTRAP-OK" "probe_sync SYNC-OK" "probe_extplayer EXTPLAYER-OK" "probe_marks MARKS-OK" "probe_stats STATS-OK" "probe_playlists PLAYLISTS-OK" "probe_cloudmerge CLOUDMERGE-OK" "probe_importers IMPORTERS-OK" "probe_onboarding ONBOARDING-OK" "probe_locallib LOCALLIB-OK" "probe_resolver RESOLVER-OK" "probe_showdispatch SHOWDISPATCH-OK" "probe_subs SUBS-OK" "probe_segments SEGMENTS-OK" "probe_stremio STREMIO-OK" "probe_savesync SAVESYNC-OK" "probe_brand BRAND-OK" "probe_theme THEME-OK" "probe_settingstxn SETTINGSTXN-OK" "probe_trakt TRAKT-OK"; do
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

if [ "$fail" -eq 0 ]; then echo "ALL HEADLESS PROBES PASSED"; else echo "SOME HEADLESS PROBES FAILED"; fi
exit "$fail"
