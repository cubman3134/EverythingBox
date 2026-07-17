#!/usr/bin/env python3
"""Perf baseline runner (phase-2 perf track). Launches the deployed app with MMV_PERF=1 +
MMV_UITEST=1, drives the standard route via the uitest pipe, then parses perf_trace.log
into a ranked markdown table.

Usage: perfbaseline.py run --exe C:/MyMediaVault-app/<name>.exe --log <dataDir>/perf_trace.log --out docs/superpowers/perf/<date>-baseline.md

Standard route: cold start -> home settles -> Games -> Recent (local ROMs) -> 50-row scroll ->
open first game (local Jurassic Park) -> wait 5s -> back out -> exit. Route steps that fail are
recorded as SKIPPED in the output, never silently dropped.

Route was fixed up against the REAL themed-XMB layout by driving it with `uitest.py state` first
(perf-task-4). The skeleton's key sequence was the SHAPE; every deviation below is a reality fixup
with an inline comment. Verified end screen (every run): themedCategory=Games, themedSelection=Recent,
themedView=home.
"""
import argparse, os, subprocess, sys, time
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uitest  # the existing pipe client


def wait_pipe(timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        try:
            uitest.state(); return True
        except Exception:
            time.sleep(0.5)
    return False


def key(k, n=1, delay=0.15):
    for _ in range(n):
        uitest._send(f"key {k}"); time.sleep(delay)


def _state():
    # uitest.state() decodes the pipe as utf-8 and returns a dict; safe even though the themed
    # category list contains a non-cp1252 glyph (which only broke the CLI's stdout print path).
    try:
        return uitest.state()
    except Exception:
        return {}


def run_route(exe, log):
    if os.path.exists(log): os.remove(log)          # fresh trace per run
    env = dict(os.environ, MMV_PERF="1", MMV_UITEST="1")
    proc = subprocess.Popen([exe], env=env, cwd=os.path.dirname(exe))
    skipped = []
    try:
        if not wait_pipe(): raise SystemExit("app pipe never came up")
        time.sleep(3)                                # home settle (startup.total ends itself;
                                                     # the initial themed-home thumbs.page also lands here)

        # FIXUP 1 (vs skeleton `down 2; right 3`): the themed XMB axes are horizontal=category,
        # vertical=item. Cold home lands on category Video / item Movies (idx 0). Games is exactly
        # ONE `right` on the category axis; no `down` is needed to change category.
        key("right")                                 # -> category Games (rail default item: Recent)

        # FIXUP 2 (vs skeleton "first console"): entering the Games rail's default item (Recent,
        # idx 0) drills into the Recent list of LOCAL ROMs (Jurassic Park at idx 0). A console
        # grid's first tile is not guaranteed to be a locally-launchable ROM; Recent is the
        # reliable local-launch path proven in perf-tasks 2/3. This also fires a thumbs.page for
        # the Recent grid.
        key("enter"); time.sleep(1.5)                # -> Recent list, Jurassic Park idx 0

        # 50-row scroll: exercises nav.select on every arrow press (+ any thumbs.page paging).
        # Kept symmetric (down 50 / up 50) so we ALWAYS return to idx 0 (Jurassic Park) before the
        # open step regardless of how many entries Recent holds -> the "open first game" step is
        # deterministic. Presses past the list end are orphan begins (no log line), harmless.
        key("down", 50, delay=0.08)
        key("up", 50, delay=0.05)

        # FIXUP 3 (vs skeleton single `enter` to open): Enter on a game opens a NavMenu overlay
        # with "Play" preselected; a SECOND enter triggers Play and actually launches the core
        # (page flips to RetroView, firing open.game). We verify each stage and record SKIPPED if
        # the overlay never appears or the core never comes up.
        launched = False
        try:
            key("enter"); time.sleep(1.0)            # open the per-game NavMenu overlay
            st = _state()
            if not st.get("overlay"):                # overlay didn't appear -> can't launch
                skipped.append("open-game (no NavMenu overlay)")
            else:
                key("enter"); time.sleep(5)          # Play -> launch core, then wait 5s
                st = _state()
                if st.get("page") == "RetroView":
                    launched = True
                else:
                    skipped.append("open-game (core did not reach RetroView)")
        except Exception:
            skipped.append("open-game (exception)")

        # FIXUP 4 (vs skeleton `escape; enter` esc-menu dance): a SINGLE `escape` exits the running
        # core straight back to themed home Games -> Recent (rail level); there is no esc-menu
        # confirm to `enter` through. If the game launched we escape to exit; then normalize.
        if launched:
            key("escape"); time.sleep(2)

        # FIXUP 5 (vs skeleton `escape 3` "back to root"): do NOT spam escape from the themed home
        # rail -- extra escapes on classic paths can pop/quit the app. Instead normalize to a
        # KNOWN end screen: press escape only while still inside RetroView (max 3), so every run
        # ends on themed home (Games / Recent), comparable run-to-run.
        for _ in range(3):
            if _state().get("page") == "RetroView":
                key("escape"); time.sleep(1.5)
            else:
                break
        # J21: the docstring promises every run ends on themedCategory=Games / themedSelection=Recent. Assert it
        # so a route that drifted off (e.g. an extra escape popped a level, or the launch never came back) fails
        # LOUDLY as a SKIPPED note instead of silently emitting a baseline that isn't comparable run-to-run.
        end = _state()
        if end.get("themedCategory") != "Games":
            skipped.append("end-state drift (themedCategory=%r, expected 'Games')" % end.get("themedCategory"))
        elif end.get("themedSelection") not in (None, "Recent"):
            skipped.append("end-state drift (themedSelection=%r, expected 'Recent')" % end.get("themedSelection"))
    finally:
        time.sleep(1)
        proc.kill()
    return skipped


def parse(log):
    spans = defaultdict(list)
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = [p.strip() for p in line.split("|")]
            if len(parts) >= 3:
                try: spans[parts[1]].append(int(parts[2]))
                except ValueError: pass
    return spans


def emit(spans, skipped, out):
    rows = sorted(((max(v), sum(v) // len(v), len(v), k) for k, v in spans.items()), reverse=True)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(f"# Perf baseline - {time.strftime('%Y-%m-%d %H:%M')}\n\n")
        f.write("Ranked by worst single occurrence on the standard route.\n\n")
        f.write("| span | worst ms | avg ms | samples |\n|---|---|---|---|\n")
        for worst, avg, n, name in rows:
            f.write(f"| {name} | {worst} | {avg} | {n} |\n")
        if skipped: f.write("\nSKIPPED route steps: " + ", ".join(skipped) + "\n")
    print(f"wrote {out} ({len(rows)} spans)")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--exe", required=True); r.add_argument("--log", required=True)
    r.add_argument("--out", required=True)
    a = ap.parse_args()
    skipped = run_route(a.exe, a.log)
    emit(parse(a.log), skipped, a.out)


if __name__ == "__main__":
    main()
