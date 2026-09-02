#!/usr/bin/env python3
"""Client for EB's UI-test channel (src/core/UiTestServer) - drive and inspect the app WITHOUT
bringing it to the front or giving it focus. The app must be running with EB_UITEST=1 (or --uitest).

Usage:
  uitest.py status                         "ok ready" once the window exists, "ok starting" before it
  uitest.py state                          print the UI state JSON
  uitest.py key down                       inject one nav key (up/down/left/right/enter/back/escape)
  uitest.py keys "down down enter"         inject a sequence (50ms apart)
  uitest.py shot C:/tmp/screen.png         save a screenshot of the window (works while occluded)
  uitest.py walk N [key]                   press a key N times (default: down), printing state each step
  uitest.py open C:/path/to/doc.pdf        open a document/book/comic by path (reader tests)
  uitest.py send "click 640 360"           synthesize a real LEFT click at window coords (X, Y)
  uitest.py send "click 640 360 right"     ...and a RIGHT click, which is the classic grid's per-item
                                           context menu (a different code path, not a flag on the left one)
  uitest.py touch tap 640 360              synthesize a real touch tap at window coords (X, Y)
  uitest.py touch flick 640 600 640 200    a drag/flick from (X1,Y1) to (X2,Y2) [add MS for duration]
  uitest.py touch pinch 640 360 2.0        two fingers around (CX,CY) diverging by SCALE [add MS]
  uitest.py touch hold 640 360 800         press and HOLD at (X,Y) for MS (default 800) then release
  uitest.py touch dtap 640 360 200         two taps at (X,Y) GAP ms apart (default 200) as ONE sequence
                                           (X/Y are LOGICAL window coords, as reported by state's "size";
                                            a touch while a gesture is still in flight replies "err busy" —
                                            wait for the previous gesture to finish, then retry)
  uitest.py send "open C:/x.cbz"           raw passthrough of any server command

No third-party deps. Windows: named pipe \\\\.\\pipe\\EverythingBox-uitest; elsewhere: the QLocalServer
unix socket (typically /tmp/EverythingBox-uitest).

Reads block on a 5s client-side timeout by default (EB_UITEST_TIMEOUT overrides it, seconds; <= 0 disables):
a connect that succeeds but never gets a reply exits 3 with "connected but no reply ... the GUI thread is
blocked", distinguishing a wedged app from a missing channel (a failed connect) or a clean protocol error.

Everything this client prints is UTF-8 (see use_utf8_streams): the app's labels are full of non-ASCII
(the detail view's "▶ Play", the settings rows' cloud/plus/pencil/star glyphs, emoji profile avatars,
em-dashes in theme names, and media titles in any language), and they must survive verbatim so a test can
assert on them.
"""
import codecs
import io
import json
import os
import sys
import threading
import time

# EB_UITEST_PIPE picks a non-default channel, matching the server-side override in UiTestServer — lets a
# test build be driven while a normally-running instance owns the default pipe.
NAME = os.environ.get("EB_UITEST_PIPE", "EverythingBox-uitest")

# Client-side read timeout (seconds). The channel now listens EARLY (issue #172), so `connect` succeeds
# the moment the socket/pipe exists — but if the GUI thread is wedged in straight-line startup the reply
# never comes and `readline()` would block forever, so a hung harness looks identical to a slow one. Bound
# the wait and fail with a distinct diagnostic instead. Override with EB_UITEST_TIMEOUT; <= 0 disables it
# and restores the old block-forever behaviour.
try:
    READ_TIMEOUT = float(os.environ.get("EB_UITEST_TIMEOUT", "5"))
except ValueError:
    READ_TIMEOUT = 5.0

TIMEOUT_EXIT = 3   # distinct from a clean protocol failure (1) or a usage error (2)


def _is_utf8(enc) -> bool:
    if not enc:
        return False
    try:
        return codecs.lookup(enc).name == "utf-8"
    except LookupError:
        return False


def use_utf8_streams() -> None:
    """Force sys.stdout/sys.stderr to UTF-8 so app text prints verbatim, whatever the caller's console is.

    Why this is needed at all: only a REAL Windows console gets UTF-8 for free (CPython opens it as
    _WindowsConsoleIO and writes wide chars via WriteConsoleW). The moment stdout is redirected — a pipe,
    a file, `x = $(uitest.py state)`, or any subprocess.run(capture_output=True), which is how automation
    actually calls this — CPython falls back to the locale encoding, cp1252 on a US/Western Windows box.
    Printing e.g. "▶ Play" (U+25B6) then raises UnicodeEncodeError from encodings/cp1252.py, and the
    failure reads like a harness/pipe fault rather than an encoding one. See issue #36.

    UTF-8 can encode every character the app can hand us, so this is lossless: no glyph is stripped or
    substituted, and `uitest.py state | grep '☁ Restore from Google Drive'` stays a writable assertion.
    (errors="backslashreplace" is a belt-and-braces guard for lone surrogates, the only thing UTF-8
    cannot encode; it escapes rather than deletes, and no app-sourced text can reach it.)
    """
    for name in ("stdout", "stderr"):
        stream = getattr(sys, name, None)
        if stream is None:                      # pythonw / fully detached: nothing to fix
            continue
        if _is_utf8(getattr(stream, "encoding", None)):
            continue                            # already UTF-8 (real console, PYTHONIOENCODING, -X utf8)
        try:
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
        except (AttributeError, io.UnsupportedOperation, ValueError):
            # Not a TextIOWrapper (pytest capture, a StringIO shim, ...). Re-wrap the raw byte stream if
            # there is one; if there isn't, the stream can't take bytes anyway, so leave it alone.
            buf = getattr(stream, "buffer", None)
            if buf is None:
                continue
            try:
                setattr(sys, name, io.TextIOWrapper(
                    buf, encoding="utf-8", errors="backslashreplace", line_buffering=True))
            except Exception:
                pass


def _timeout_exit() -> None:
    # Distinct from a clean failure: the channel WAS reachable, we just never got a line back.
    # os._exit, not SystemExit: on Windows the abandoned reader thread is still blocked inside a
    # kernel read on the pipe handle, and unwinding out of the `with open(...)` would call close()
    # on that same handle, which itself blocks until the read returns — i.e. until the wedged app
    # finally answers or dies. That is the exact hang we are trying to escape, so flush and exit now.
    sys.stderr.write(f"connected but no reply within {READ_TIMEOUT:g}s — the GUI thread is blocked\n")
    sys.stderr.flush()
    try:
        sys.stdout.flush()
    except Exception:
        pass
    os._exit(TIMEOUT_EXIT)


def _readline_timeout(readline, timeout: float) -> bytes:
    """Run a blocking readline() with a wall-clock cap. Windows named pipes have no per-read timeout
    without pywin32, so read in a daemon thread and abandon it if it overruns — the wedged GUI never
    replies, so there is nothing to recover, and a daemon thread dies with the process."""
    box = {}

    def worker():
        try:
            box["line"] = readline()
        except Exception as exc:                # surface a real read error over a false timeout
            box["error"] = exc

    t = threading.Thread(target=worker, daemon=True)
    t.start()
    t.join(timeout)
    if t.is_alive():
        _timeout_exit()
    if "error" in box:
        raise box["error"]
    return box.get("line") or b""


def _send(cmd: str) -> str:
    if os.name == "nt":
        with open(rf"\\.\pipe\{NAME}", "r+b", buffering=0) as f:
            f.write((cmd + "\n").encode("utf-8"))
            if READ_TIMEOUT > 0:
                line = _readline_timeout(f.readline, READ_TIMEOUT)
            else:
                line = f.readline()
            return line.decode("utf-8", "replace").strip()
    import socket
    path = f"/tmp/{NAME}"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(path)
        s.sendall((cmd + "\n").encode("utf-8"))
        if READ_TIMEOUT > 0:
            s.settimeout(READ_TIMEOUT)          # applies to the reply only; connect already succeeded
        buf = b""
        while not buf.endswith(b"\n"):
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                _timeout_exit()
            if not chunk:
                break
            buf += chunk
        return buf.decode("utf-8", "replace").strip()


def state() -> dict:
    resp = _send("state")
    if not resp.startswith("ok "):
        raise SystemExit(f"state failed: {resp}")
    return json.loads(resp[3:])


def main() -> int:
    use_utf8_streams()   # before ANY print: app labels are non-ASCII and cp1252 stdout would raise (#36)
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "status":
        # Answerable with no main window, so it is the one command that distinguishes "the app is still
        # starting" from "the app is up" — and both of those from "there is no channel here at all", which
        # is what a failed connect below would raise.
        print(_send("status"))
    elif cmd == "state":
        print(json.dumps(state(), indent=2, ensure_ascii=False))
    elif cmd == "key":
        print(_send(f"key {sys.argv[2]}"))
    elif cmd == "keys":
        for k in sys.argv[2].split():
            r = _send(f"key {k}")
            if r != "ok":
                print(f"{k}: {r}")
                return 1
            time.sleep(0.05)
        print("ok")
    elif cmd == "shot":
        print(_send(f"shot {os.path.abspath(sys.argv[2])}"))
    elif cmd == "open":
        # Open a document/book by path through the reader-test hook (src/core/UiTestServer -> openDocumentPath).
        print(_send(f"open {os.path.abspath(sys.argv[2])}"))
    elif cmd == "touch":
        # Synthesize a real touch gesture: `touch tap X Y`, `touch flick X1 Y1 X2 Y2 [MS]`,
        # `touch pinch CX CY SCALE [MS]`. The rest of argv is passed through verbatim to the server.
        print(_send("touch " + " ".join(sys.argv[2:])))
    elif cmd == "send":
        # Raw passthrough of any server command (e.g. `send "open C:/x.pdf"`), for ad-hoc reader driving.
        print(_send(sys.argv[2]))
    elif cmd == "walk":
        n = int(sys.argv[2])
        key = sys.argv[3] if len(sys.argv) > 3 else "down"
        for i in range(n):
            _send(f"key {key}")
            time.sleep(0.05)
            s = state()
            # Most specific selection wins. panelFocus/themedSelection were missing, so on the themed
            # panel host and the themed home — i.e. most of the app — every step printed the useless
            # "QQuickWidget" (the opaque focus CLASS) instead of the row label a test wants to assert on.
            sel = (s.get("overlaySelection") or s.get("panelFocus") or s.get("themedSelection")
                   or s.get("focusText") or s.get("focus"))
            print(f"{i + 1:2d}. {sel}")
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
