#!/usr/bin/env python3
"""Polish-track flow sweep (phase-2 polish track, Task 1). Launches the deployed app with
MMV_UITEST=1, walks the themed-XMB home flow, and captures a screenshot set that Task 2 ranks
for jarring transitions / rough empty states. Drives ONLY through the uitest pipe (no focus /
SendKeys); grab() renders the window even while occluded.

Usage: polishsweep.py run --exe C:/MyMediaVault-app/MyMediaVault.exe --out .superpowers/polish-audit

Output naming contract (Task 2 depends on it):
  step-NN-<slug>.png            a settled still of a named step
  trans-NN-<slug>-a.png / -b.png  a page-CHANGE pair: -a is the frame right after the key send,
                                  -b is ~200 ms later (400 ms for metadata-settle pairs). A hard
                                  cut shows two unrelated frames with no intermediate; a smooth one
                                  shows an in-between. NN is a single monotonic sequence across BOTH
                                  stills and pairs, so Task 2 can order the whole walk chronologically.
Every named step produces its file(s) or a `SKIPPED: <why>` line in notes.md -- never silently dropped.

The themed-XMB nav map was verified live with `uitest.py state` before this script was written
(same discipline as perfbaseline.py). Reality, not guesses:
  - Categories left->right from cold root: Video, Games, Audio, Reading, Profiles, Settings.
  - horizontal = category, vertical = item within a category's rail. One `right` from root -> Games.
  - Games rail default item Recent -> Enter drills the local-ROM list (Jurassic Park at idx 0).
  - Enter on a game opens a NavMenu overlay ("Play" preselected); a SECOND Enter launches (page ->
    RetroView). A SINGLE Escape exits the core back to themed home.
  - Escape on a rail (not inside a folder) opens the esc NavMenu ("Resume"); Escape again closes it.
  - Search: the OSK opens on the raw Qt::Key_Slash value (47); Escape cancels it.
  - Weekend Picks (user data, protected -- never modified) lives at Video > Movies > Playlists (idx 0).
Reuses perfbaseline.py's wait_pipe / key / finally-kill patterns.
"""
import argparse, json, os, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uitest  # the existing pipe client

KEY_SLASH = 47  # Qt::Key_Slash -- opens the search OSK (uitest 'key' accepts a raw Qt::Key int)

_seq = 0
_notes = []
_outdir = "."


def send(cmd, timeout=8):
    """uitest._send with a hard wall-clock timeout. A themed detail page starts an mpv VIDEO PREVIEW
    that can bog the GUI thread; without a timeout a single pipe read there stalls the whole sweep for
    minutes. On timeout/error we return "" so the caller degrades to a SKIPPED note and keeps walking.
    The blocked read is abandoned on a daemon thread; the next call opens a fresh pipe connection
    (QLocalServer accepts many), so one slow step never blocks the following ones."""
    box = {}
    def worker():
        try: box["r"] = uitest._send(cmd)
        except Exception as e: box["e"] = e
    t = threading.Thread(target=worker, daemon=True); t.start(); t.join(timeout)
    return box.get("r", "")


def wait_pipe(timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        if send("state", timeout=4).startswith("ok"):
            return True
        time.sleep(0.5)
    return False


def _state():
    r = send("state")
    if r.startswith("ok "):
        try: return json.loads(r[3:])
        except Exception: return {}
    return {}


def key(k, n=1, delay=0.15):
    for _ in range(n):
        send(f"key {k}"); time.sleep(delay)


def _shot(path):
    return send(f"shot {os.path.abspath(path)}").startswith("ok")


def _desc(st):
    # A compact, ascii-safe one-liner of what the state JSON says is on screen (encode-safe: the
    # themed category/selection can carry non-cp1252 glyphs that would crash a raw stdout print).
    parts = []
    for f in ("page", "themedCategory", "themedSelection", "themedView", "overlay",
              "overlaySelection", "escMenu"):
        v = st.get(f)
        if v not in (None, "", False):
            parts.append(f"{f}={v}")
    s = " ".join(parts)
    return s.encode("ascii", "replace").decode("ascii")


def still(slug, msg=""):
    """A settled still of a named step."""
    global _seq
    _seq += 1
    name = f"step-{_seq:02d}-{slug}.png"
    ok = _shot(os.path.join(_outdir, name))
    st = _state()
    if ok:
        _notes.append(f"{name}: {_desc(st)}{('  -- ' + msg) if msg else ''}")
    else:
        _notes.append(f"SKIPPED {slug} (step-{_seq:02d}): screenshot failed{('  -- ' + msg) if msg else ''}")
    return st


def trans(slug, keyfn, gap=0.2, msg=""):
    """A page-CHANGE pair: fire the key, grab -a at once, -b after `gap` s. `keyfn` does the key send(s)."""
    global _seq
    _seq += 1
    base = f"trans-{_seq:02d}-{slug}"
    keyfn()
    a = _shot(os.path.join(_outdir, base + "-a.png"))
    time.sleep(gap)
    b = _shot(os.path.join(_outdir, base + "-b.png"))
    st = _state()
    if a and b:
        _notes.append(f"{base}-a/b (gap {int(gap*1000)}ms): {_desc(st)}{('  -- ' + msg) if msg else ''}")
    else:
        _notes.append(f"SKIPPED {slug} (trans-{_seq:02d}): shot a={a} b={b}{('  -- ' + msg) if msg else ''}")
    return st


def skip(slug, why):
    global _seq
    _seq += 1
    _notes.append(f"SKIPPED {slug} (seq-{_seq:02d}): {why}")


def _in_overlay(st):
    # Any modal that must be dismissed before XMB nav is meaningful. The QML action menu (Play/Favorite/
    # Add to playlist/Download) is a QML overlay, NOT a NavOverlay widget, so it is invisible to the
    # `overlay` field -- it shows only as themedFocus="action:N". Miss it and the next Enter fires "Play"
    # (fullscreen video) -- the exact bug that stalled the first sweep run.
    return (st.get("overlay") or st.get("escMenu")
            or str(st.get("themedFocus", "")).startswith("action:")
            or st.get("page") == "RetroView")


def to_root():
    """Normalise to cold root: (1) dismiss any modal, (2) unwind any drilled sub-list back to the top
    category rail, (3) park on Video (idx0)."""
    # (1) close any modal (action menu / OSK / NavMenu / esc menu / running core).
    for _ in range(6):
        st = _state()
        if _in_overlay(st):
            key("escape"); time.sleep(0.4)
        else:
            break
    # (2) Unwind a drilled sub-list (e.g. Video>Movies>subrail) back to the top rail. themedView stays
    # "home" at EVERY depth, so depth is unreadable from state -- instead press escape and watch for the
    # esc menu, which appears ONLY at the top rail. Seeing it means we were already fully out: close it, done.
    for _ in range(6):
        key("escape"); time.sleep(0.4)
        st = _state()
        if st.get("escMenu") or st.get("overlay"):
            key("escape"); time.sleep(0.4)   # esc menu -> we're at the top rail; close it and stop
            break
    # (3) park on Video (idx0); 6 categories wide, left-spam is idempotent at the left end.
    key("left", 6, delay=0.12); time.sleep(0.3)


def run(exe, outdir):
    global _outdir
    _outdir = outdir
    os.makedirs(outdir, exist_ok=True)
    env = dict(os.environ, MMV_UITEST="1")
    proc = subprocess.Popen([exe], env=env, cwd=os.path.dirname(exe))
    try:
        if not wait_pipe():
            raise SystemExit("app pipe never came up")
        time.sleep(3)  # home settle

        # ---- Step 1a: XMB root still, then each category left-to-right (a still per category). Each
        # `right` slides the XMB one category -> a page change -> capture the trans pair AND a settled still.
        to_root()
        still("home-root", "cold themed home")
        for slug in ("cat-games", "cat-audio", "cat-reading", "cat-profiles", "cat-settings"):
            trans(slug, lambda: key("right"))
            still(slug)

        # ---- Step 1b: into Games > Recent (local ROMs). Return to Games first (we're on Settings).
        key("left", 4, delay=0.12); time.sleep(0.3)   # Settings(5) -> Games(1)
        st = _state()
        if st.get("themedCategory") != "Games":
            key("left", 6, delay=0.12); key("right"); time.sleep(0.3)  # hard reset -> Games
        trans("enter-recent", lambda: key("enter"), msg="Games>Recent drill")
        st = still("games-recent", "Recent local-ROM list, Jurassic Park idx0")
        if st.get("themedSelection") != "Jurassic Park":
            _notes.append(f"NOTE: Recent head is '{_desc(st)}' not Jurassic Park (route still valid).")

        # ---- Step 1c: item selection +/-3 rows with metadata settling (grab at 0 ms AND 400 ms per row).
        for i in range(1, 4):
            trans(f"row-down{i}", lambda: key("down"), gap=0.4, msg="metadata settle 0ms->400ms")
        still("row-down3-settled", "3 rows down, panel settled")
        key("up", 3, delay=0.2); time.sleep(0.3)      # back to idx0 (Jurassic Park) deterministically
        still("row-back-top", "returned to Recent head")

        # ---- Step 1d: open a game's NavMenu overlay, then cancel it.
        st = trans("navmenu-open", lambda: key("enter"), msg="per-game NavMenu (Play preselected)")
        if st.get("overlay") == "NavMenu":
            still("navmenu-open", f"overlay={st.get('overlaySelection')}")
            trans("navmenu-cancel", lambda: key("escape"), msg="cancel overlay")
            still("navmenu-cancelled")
        else:
            skip("navmenu", f"NavMenu overlay did not appear (state: {_desc(st)})")

        # ---- Step 1e: drill a non-game category. Video > Movies > (movie detail if the backend is up).
        to_root()
        st = _state()
        if st.get("themedCategory") == "Video" and st.get("themedSelection") == "Movies":
            trans("movies-enter", lambda: key("enter"), msg="Video>Movies sub-rail")
            time.sleep(1.5)
            st = still("movies-subrail", "Movies sub-rail (Recent|Playlists|<movies>)")
            # step down past Recent/Playlists onto a real movie tile, then open its detail page.
            key("down", 2, delay=0.35); time.sleep(0.4)
            st = _state()
            movie = st.get("themedSelection")
            trans("movie-detail", lambda: key("enter"), gap=0.4, msg=f"open detail for '{movie}'")
            time.sleep(1.5)
            st = _state()
            # Enter on a movie opens the QML action menu (Play/Favorite/Add to playlist/Download) over
            # the detail panel + an mpv preview -- surfaced as themedFocus="action:N", not the overlay field.
            if str(st.get("themedFocus", "")).startswith("action:"):
                still("movie-detail", f"action menu + detail for movie '{movie}' (Play preselected)")
            else:
                skip("movie-detail", f"no detail/action menu surfaced (backend down?) state: {_desc(st)}")
            to_root()  # closes the action menu (escape) before the next step -- must NOT leave Play armed
        else:
            skip("movies-drill", f"not on Video>Movies at root (state: {_desc(st)})")

        # ---- Step 1f: Playlists folder > Weekend Picks (READ ONLY -- never modify user data).
        to_root()
        # Weekend Picks is a MOVIE playlist: Video > Movies > Playlists (idx1) > Weekend Picks (idx0).
        trans("movies-enter2", lambda: key("enter"), msg="Video>Movies (for Playlists)")
        time.sleep(1.2)
        key("down"); time.sleep(0.35)   # Recent(0) -> Playlists(1)
        st = _state()
        if st.get("themedSelection") == "Playlists":
            trans("playlists-enter", lambda: key("enter"), msg="into Playlists folder")
            time.sleep(1.0)
            st = still("playlists", "playlists list (Weekend Picks + New playlist)")
            if st.get("themedSelection") == "Weekend Picks":
                trans("weekend-picks", lambda: key("enter"), msg="open Weekend Picks (read only)")
                time.sleep(1.0)
                still("weekend-picks", "Weekend Picks contents")
                key("escape"); time.sleep(0.4)
            else:
                skip("weekend-picks", f"head not Weekend Picks (state: {_desc(st)})")
            key("escape"); time.sleep(0.4)
        else:
            skip("playlists", f"could not reach Playlists (state: {_desc(st)})")
        to_root()

        # ---- Step 1g: search -- '/' opens the OSK, Escape cancels.
        st = trans("search-osk", lambda: key(KEY_SLASH), msg="open search OSK")
        time.sleep(0.5)
        st = _state()
        if st.get("overlay") == "Osk":
            still("search-osk", "on-screen keyboard visible")
            trans("search-cancel", lambda: key("escape"), msg="cancel OSK")
            still("search-cancelled")
        else:
            skip("search-osk", f"OSK did not open on key {KEY_SLASH} (state: {_desc(st)})")
        to_root()

        # ---- Step 1h: esc menu open / close (Escape on a rail).
        key("right"); time.sleep(0.3)   # -> Games rail (a rail level, not a folder)
        st = trans("escmenu-open", lambda: key("escape"), msg="esc menu")
        if st.get("escMenu") or st.get("overlay") == "NavMenu":
            still("escmenu-open", f"esc menu ({st.get('overlaySelection')})")
            trans("escmenu-close", lambda: key("escape"), msg="close esc menu")
            still("escmenu-closed")
        else:
            skip("escmenu", f"esc menu did not open (state: {_desc(st)})")

        # ---- Step 1i: launch the local game (Jurassic Park) and capture the RetroView entry cut pair.
        to_root(); key("right"); time.sleep(0.3)    # Games rail, item Recent
        trans("enter-recent2", lambda: key("enter"), msg="Games>Recent")
        time.sleep(1.2)
        st = _state()
        launched = False
        if st.get("themedSelection") == "Jurassic Park":
            trans("navmenu-launch", lambda: key("enter"), msg="open NavMenu to launch")
            time.sleep(0.8)
            st = _state()
            if st.get("overlay") == "NavMenu":
                # THE cut pair the perf/polish tracks care about: home -> RetroView on Play.
                trans("retroview-enter", lambda: key("enter"), gap=0.2, msg="Play -> RetroView entry cut")
                time.sleep(4)
                st = _state()
                if st.get("page") == "RetroView":
                    launched = True
                    still("retroview", "core running (may grab black for video surface)")
                else:
                    skip("retroview", f"core did not reach RetroView (state: {_desc(st)})")
            else:
                skip("navmenu-launch", f"launch overlay missing (state: {_desc(st)})")
        else:
            skip("launch", f"Recent head not Jurassic Park (state: {_desc(st)})")

        # ---- Step 1j: Escape back out of the core -- capture the exit cut pair.
        if launched:
            trans("retroview-exit", lambda: key("escape"), gap=0.2, msg="core -> themed home exit cut")
            time.sleep(2)
            still("home-after-exit", "back on themed home after core exit")
            # normalise: escape only while still in RetroView (max 3), so we always end on home.
            for _ in range(3):
                if _state().get("page") == "RetroView":
                    key("escape"); time.sleep(1.5)
                else:
                    break
        else:
            skip("retroview-exit", "core never launched, nothing to exit")

    except Exception as e:
        _notes.append(f"SWEEP ABORTED (unexpected): {type(e).__name__}: {e}")
    finally:
        time.sleep(1)
        proc.kill()
        write_notes(outdir)


def write_notes(outdir):
    path = os.path.join(outdir, "notes.md")
    stills = sum(1 for n in _notes if n.startswith("step-"))
    pairs = sum(1 for n in _notes if n.startswith("trans-"))
    skipped = [n for n in _notes if n.startswith("SKIPPED")]
    header_note = None
    if os.path.exists(path):
        # keep any hand-written empty-state matrix appended below the auto section
        with open(path, encoding="utf-8") as f:
            existing = f.read()
        marker = "<!-- MANUAL BELOW -->"
        if marker in existing:
            header_note = existing[existing.index(marker):]
    with open(path, "w", encoding="utf-8") as f:
        f.write(f"# Polish sweep - {time.strftime('%Y-%m-%d %H:%M')}\n\n")
        f.write(f"Auto flow-sweep by polishsweep.py. Stills: {stills}. Transition pairs: {pairs} "
                f"(x2 files). SKIPPED: {len(skipped)}.\n\n")
        f.write("Each line: file(s) -> what the uitest `state` JSON reported on screen.\n\n")
        for n in _notes:
            f.write(f"- {n}\n")
        if skipped:
            f.write("\n## SKIPPED\n\n")
            for n in skipped:
                f.write(f"- {n}\n")
        if header_note:
            f.write("\n" + header_note)
    print(f"wrote {path}: {stills} stills, {pairs} trans pairs, {len(skipped)} skipped")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--exe", default=r"C:/MyMediaVault-app/MyMediaVault.exe")
    r.add_argument("--out", default=".superpowers/polish-audit")
    a = ap.parse_args()
    run(a.exe, a.out)


if __name__ == "__main__":
    main()
