#!/usr/bin/env python3
"""Client for MMV's UI-test channel (src/core/UiTestServer) - drive and inspect the app WITHOUT
bringing it to the front or giving it focus. The app must be running with MMV_UITEST=1 (or --uitest).

Usage:
  uitest.py state                          print the UI state JSON
  uitest.py key down                       inject one nav key (up/down/left/right/enter/back/escape)
  uitest.py keys "down down enter"         inject a sequence (50ms apart)
  uitest.py shot C:/tmp/screen.png         save a screenshot of the window (works while occluded)
  uitest.py walk N [key]                   press a key N times (default: down), printing state each step

No third-party deps. Windows: named pipe \\\\.\\pipe\\MyMediaVault-uitest; elsewhere: the QLocalServer
unix socket (typically /tmp/MyMediaVault-uitest).
"""
import json
import os
import sys
import time

NAME = "MyMediaVault-uitest"


def _send(cmd: str) -> str:
    if os.name == "nt":
        with open(rf"\\.\pipe\{NAME}", "r+b", buffering=0) as f:
            f.write((cmd + "\n").encode("utf-8"))
            return f.readline().decode("utf-8", "replace").strip()
    import socket
    path = f"/tmp/{NAME}"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(path)
        s.sendall((cmd + "\n").encode("utf-8"))
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(4096)
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
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "state":
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
    elif cmd == "walk":
        n = int(sys.argv[2])
        key = sys.argv[3] if len(sys.argv) > 3 else "down"
        for i in range(n):
            _send(f"key {key}")
            time.sleep(0.05)
            s = state()
            sel = s.get("overlaySelection") or s.get("focusText") or s.get("focus")
            print(f"{i + 1:2d}. {sel}")
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
