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
"$PY" "$RELAY_PY" --port "$RELAY_PORT" > /tmp/mmv-relay.log 2>&1 &
RELAY_PID=$!
trap '[ -n "${RELAY_PID:-}" ] && kill "$RELAY_PID" 2>/dev/null' EXIT
for _ in $(seq 1 40); do grep -q "listening" /tmp/mmv-relay.log 2>/dev/null && break; sleep 0.2; done
echo "relay: $(cat /tmp/mmv-relay.log 2>/dev/null | head -1)"; echo

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

if [ "$fail" -eq 0 ]; then echo "ALL HEADLESS PROBES PASSED"; else echo "SOME HEADLESS PROBES FAILED"; fi
exit "$fail"
