#!/usr/bin/env python3
"""A FIXTURE AUDIOBOOKSHELF SERVER, for driving the app by hand (issue #197).

Not a mock library and not a test double the app links: an ordinary HTTP server on a loopback port,
answering the seven endpoints the client speaks with a small, fixed collection. It exists because a real
Audiobookshelf server was not available while this feature was written, and "it browsed a struct I made up"
is not evidence that a client works.

WHAT IT IS FOR, and what it is not. probe_absclient has a stub of its own, in-process, and that is what CI
runs — this one is for a HUMAN driving the real application: add the server through the real Settings row,
walk the real browse levels, press play on the real player, and watch the PATCH arrive. The two serve the
same fixture on purpose, so a discrepancy between them is a discrepancy worth looking at.

    python native/tools/abs_stub.py [--port 13378]

Then add http://127.0.0.1:13378 as an audiobook server, with any username and any password: this fixture
accepts every sign-in and issues one token. NOTHING HERE IS A CREDENTIAL — the token below is the literal
string "probe-fixture-token-6f3a9c2e" and the audio it serves is silence it generates itself.

It prints every request it is asked, with the Authorization header REDACTED, and prints the body of every
progress PATCH — which is the thing worth watching, because "the position reached the server" is the one
claim in this feature that cannot be seen from inside the app.
"""
import argparse
import json
import math
import struct
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN = "probe-fixture-token-6f3a9c2e"

# The server's own progress, per item — the state a real Audiobookshelf keeps and every client of it agrees
# on. 110 s is inside PART TWO (100..300), ten seconds in, so a first open has to land on the second part
# AND ten seconds into it — which is what makes "the resume came from the server" visible
# on the very first press rather than only after a round trip.
PROGRESS = {"li_multi": {"currentTime": 110.0, "duration": 450.0, "isFinished": False}}

TRACKS = [
    {"index": 1, "startOffset": 0,   "duration": 100, "title": "01 - One.mp3",
     "contentUrl": "/api/items/li_multi/file/af_1", "mimeType": "audio/wav"},
    {"index": 2, "startOffset": 100, "duration": 200, "title": "02 - Two.mp3",
     "contentUrl": "/api/items/li_multi/file/af_2", "mimeType": "audio/wav"},
    {"index": 3, "startOffset": 300, "duration": 150, "title": "03 - Three.mp3",
     "contentUrl": "/api/items/li_multi/file/af_3", "mimeType": "audio/wav"},
]
CHAPTERS = [
    {"id": 0, "start": 0,   "end": 80,  "title": "Chapter One"},
    {"id": 1, "start": 80,  "end": 260, "title": "Chapter Two"},
    {"id": 2, "start": 260, "end": 450, "title": "Chapter Three"},
]


def book(item_id, title, author, tracks, series=None, seq=None):
    md = {"title": title, "authorName": author}
    if series:
        md["seriesName"] = series
        md["sequence"] = seq
    return {"id": item_id, "mediaType": "book",
            "media": {"duration": sum(t["duration"] for t in tracks), "numTracks": len(tracks),
                      "coverPath": "/covers/%s.jpg" % item_id, "metadata": md,
                      "tracks": tracks, "chapters": CHAPTERS}}


LIBRARIES = [{"id": "lib_books", "name": "Books", "mediaType": "book"},
             {"id": "lib_pods", "name": "Shows", "mediaType": "podcast"}]

BOOKS = [book("li_multi", "The Long Book", "A. Writer", TRACKS, "Chronicles", "2"),
         book("li_one", "One File", "A. Writer", [dict(TRACKS[0], duration=60)]),
         book("li_other", "Another", "B. Author", [dict(TRACKS[0], duration=90)], "Chronicles", "1")]

PODCAST = {"id": "li_pod", "mediaType": "podcast",
           "media": {"numEpisodes": 2, "coverPath": "/covers/pod.jpg",
                     "metadata": {"title": "A Show", "authorName": "Host"},
                     "episodes": [
                         {"id": "ep_old", "title": "The first one", "pubDate": "2024-01-02T00:00:00Z",
                          "audioFile": {"duration": 45}},
                         {"id": "ep_new", "title": "The latest one", "pubDate": "2025-06-01T00:00:00Z",
                          "duration": 60}]}}


def silence_wav(seconds, rate=1000):
    """A real, playable WAV of silence. Generated rather than shipped: a binary fixture in the tree is a
    binary fixture to review, and mpv only has to be able to open it and report a duration.

    THE SAMPLE RATE IS ABSURDLY LOW ON PURPOSE, and it is the difference between this fixture proving
    something and quietly proving nothing. mpv's `duration` for a file it is STREAMING starts as an
    estimate from the bytes it has so far and converges as they arrive — and the host applies a resume
    seek exactly once, on the first duration it is told. At 8 kHz a 200-second part is 3.2 MB, the first
    duration reported was 126 s, and a perfectly correct 150-second resume was silently dropped as "past
    the end of the file". At 1 kHz the same part is 400 KB, lands in one read, and the first duration mpv
    reports is the real one. Nothing is being listened to here; only the length matters."""
    n = int(seconds * rate)
    data = b"\x00\x00" * n
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(data))
    return hdr + data


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass   # our own line, below, says more and says it without the token

    # ---- plumbing ----------------------------------------------------------------------------------
    def _note(self, body=None):
        auth = self.headers.get("Authorization")
        # REDACTED, always. This script prints to a terminal somebody may screenshot, and a fixture token
        # printed here is a habit that transfers to a real one.
        shown = "Bearer <redacted>" if auth else "-"
        line = "%-6s %-46s auth=%s" % (self.command, self.path, shown)
        if body:
            line += "  body=%s" % body.decode("utf-8", "replace")
        print(line, flush=True)

    def _send(self, obj, status=200, raw=None, ctype="application/json"):
        payload = raw if raw is not None else json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _send_media(self, data, ctype):
        """A media file, WITH BYTE RANGES.

        Not politeness: a real Audiobookshelf serves its files over a range-capable static handler, and mpv
        treats a stream that cannot be ranged as one whose length it has to ESTIMATE from what it has read
        so far. The host applies a resume seek exactly once, on the first duration it is told, and refuses a
        target within five seconds of the end — so against a fixture with no Range support a perfectly
        correct 130-second resume into a 200-second part was silently dropped, because at that instant mpv
        believed the part was 128 seconds long. The bug was entirely in the fixture, and it presented as the
        feature's headline claim not working."""
        rng = self.headers.get("Range") or ""
        start, end = 0, len(data) - 1
        status = 200
        if rng.startswith("bytes="):
            spec = rng[6:].split(",")[0].strip()
            a, _, b = spec.partition("-")
            if a:
                start = int(a)
                if b:
                    end = min(int(b), end)
            elif b:                       # a suffix range: the LAST n bytes
                start = max(0, len(data) - int(b))
            status = 206
        chunk = data[start:end + 1]
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Accept-Ranges", "bytes")
        if status == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, len(data)))
        self.send_header("Content-Length", str(len(chunk)))
        self.end_headers()
        self.wfile.write(chunk)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    # ---- routes ------------------------------------------------------------------------------------
    def do_POST(self):
        body = self._body()
        path = self.path.split("?")[0]
        # The password arrives here and is not printed. It is the only place one exists in this whole
        # exercise, and printing it would be exactly the mistake the feature is careful about.
        self._note(b"<login>" if path == "/login" else body)
        if path == "/login":
            return self._send({"user": {"id": "u1", "username": "reader", "token": TOKEN},
                               "userDefaultLibraryId": "lib_books"})
        if path == "/api/authorize":
            # "server-settings" is what a real Audiobookshelf answers with — a constant, not an instance
            # id — so the client keeps the uuid it minted. See Audiobookshelf.h.
            return self._send({"user": {"id": "u1"}, "serverSettings": {"id": "server-settings"}})
        if "/play" in path:
            item = path.split("/api/items/")[1].split("/")[0]
            src = next((b for b in BOOKS if b["id"] == item), None)
            if src is None and item == "li_pod":
                return self._send({"id": "sess_ep", "duration": 60, "currentTime": 0,
                                   "audioTracks": [dict(TRACKS[0], duration=60,
                                                        contentUrl="/api/items/li_pod/file/ep")],
                                   "chapters": []})
            if src is None:
                return self._send({}, 404)
            p = PROGRESS.get(item, {})
            # WITH `libraryItem`, which a real Audiobookshelf sends and which a client re-opening from its
            # own Recents has no other source for — see Audiobookshelf.h's note on Session::title.
            return self._send({"id": "sess_1", "duration": src["media"]["duration"],
                               "currentTime": p.get("currentTime", 0.0),
                               "libraryItem": src,
                               "audioTracks": src["media"]["tracks"],
                               "chapters": src["media"]["chapters"]})
        return self._send({}, 404)

    def do_PATCH(self):
        body = self._body()
        self._note(body)
        if self.path.startswith("/api/me/progress/"):
            key = self.path[len("/api/me/progress/"):]
            try:
                PROGRESS[key.split("/")[0]] = json.loads(body or b"{}")
            except ValueError:
                pass
            print("    >>> the server now holds %s" % json.dumps(PROGRESS), flush=True)
            return self._send({"ok": True})
        return self._send({}, 404)

    def do_GET(self):
        self._note()
        path = self.path.split("?")[0]
        if path == "/api/libraries":
            return self._send({"libraries": LIBRARIES})
        if path == "/api/libraries/lib_books/items":
            return self._send({"results": BOOKS})
        if path == "/api/libraries/lib_pods/items":
            return self._send({"results": [PODCAST]})
        if path == "/api/libraries/lib_books/series":
            return self._send({"results": [{"id": "ser_1", "name": "Chronicles",
                                            "books": [{"id": "li_multi"}, {"id": "li_other"}]}]})
        if path == "/api/libraries/lib_books/authors":
            return self._send({"authors": [{"id": "aut_1", "name": "A. Writer", "numBooks": 2},
                                           {"id": "aut_2", "name": "B. Author", "numBooks": 1}]})
        if path.startswith("/api/items/"):
            rest = path[len("/api/items/"):]
            if "/file/" in rest:
                # A real, openable file: silence as long as the track claims to be, so the player's
                # duration, the chapter list and the position bar all have something true to work with.
                item, _, af = rest.partition("/file/")
                src = next((b for b in BOOKS if b["id"] == item), None)
                secs = 60
                if src:
                    for t in src["media"]["tracks"]:
                        if t["contentUrl"].endswith(af):
                            secs = t["duration"]
                return self._send_media(silence_wav(secs), "audio/wav")
            if rest.endswith("/cover"):
                return self._send(None, raw=b"", ctype="image/jpeg")
            item = rest
            if item == "li_pod":
                return self._send(PODCAST)
            src = next((b for b in BOOKS if b["id"] == item), None)
            return self._send(src if src else {}, 200 if src else 404)
        if path.startswith("/api/me/progress/"):
            key = path[len("/api/me/progress/"):].split("/")[0]
            p = PROGRESS.get(key)
            return self._send(dict(p, libraryItemId=key) if p else {}, 200 if p else 404)
        return self._send({}, 404)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=13378)
    args = ap.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print("fixture Audiobookshelf on http://127.0.0.1:%d  (any username, any password)" % args.port,
          flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
