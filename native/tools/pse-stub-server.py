#!/usr/bin/env python3
"""A stand-in for a Komga library, for driving OPDS-PSE (#153) without one.

Serves the four things the feature needs and nothing else:

  GET   /opds/v1.2/series/1                       an acquisition feed, one comic entry, carrying both an
                                                  ordinary acquisition link (Download) and an OPDS-PSE
                                                  page-stream link with pse:count and pse:lastRead
  GET   /opds/v1.2/books/0A1/pages/<n>?zero_based=true
                                                  page <n> as a PNG with the page number drawn on it, so a
                                                  screenshot of the reader says which page it is showing
  GET   /opds/v1.2/books/0A1/file/saga-1.cbz      the whole volume, for the "Download" verb
  PATCH /api/v1/books/0A1/read-progress           Komga's read-progress API; the body is appended to
                                                  progress.log so a drive can show what the client sent

Everything is behind HTTP Basic (reader / hunter2) EXCEPT nothing: a request without the header gets a 401,
which is how the drive shows the credentials really are on the page requests.

Options:
  --port N            listen port (default 8975)
  --pages N           how many pages the volume has (default 12)
  --last-read N       what pse:lastRead advertises, 1-based; 0 omits the attribute (default 7)
  --max-width         advertise a {maxWidth} placeholder in the template as well
  --die-after N       serve N page images, then stall for ever on every later one — the stalled-stream case
  --state DIR         where progress.log and requests.log are written (default: beside this script)

NOT a Komga. It implements the shapes this client reads and writes and nothing else, and it exists because
a real Komga / Kavita was not available. Do not mistake a green drive against it for a live verification.
"""
import argparse
import base64
import http.server
import json
import os
import struct
import sys
import threading
import time
import zlib

ARGS = None
STATE = None


LOG_LOCK = threading.Lock()


def log(name, line):
    # Under the lock: this is a THREADING server, and two page requests appending at once lose
    # lines - which made the recorded fetch ORDER (the whole point of the log) look like it had
    # gaps in it.
    with LOG_LOCK:
        with open(os.path.join(STATE, name), 'a', encoding='utf-8') as f:
            f.write(line + '\n')


def png(width, height, page_number, total):
    """A page image with a machine-readable identity: the background is a solid colour derived from the
    page number, so a screenshot of the reader can be checked without OCR (page 7 is one exact RGB), and
    a black bar of `page_number` pixels runs down the left edge for a human reading the shot."""
    r = (page_number * 37) % 256
    g = (page_number * 91) % 256
    b = 200
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # filter: none
        for x in range(width):
            if x < 4 + page_number * 6 and y < 40:
                rows += bytes((0, 0, 0))
            else:
                rows += bytes((r, g, b))

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data
                + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr)
            + chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b''))


def cbz(pages):
    """The whole volume as a stored (uncompressed) ZIP — what the Download verb fetches."""
    import io, zipfile
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_STORED) as z:
        for i in range(pages):
            z.writestr('%05d.png' % (i + 1), png(600, 900, i + 1, pages))
    return buf.getvalue()


def last_reported():
    """The page a client last TOLD us it reached, so re-opening the feed advertises the position the
    reader reported rather than the one this stub was started with. That is what makes the round trip
    (read to page N -> close -> re-open at page N) a real one instead of a replay of a constant."""
    path = os.path.join(STATE, 'progress.log')
    if not os.path.exists(path):
        return 0
    lines = [l for l in open(path, encoding='utf-8').read().splitlines() if l.strip()]
    if not lines:
        return 0
    try:
        return int(json.loads(lines[-1]).get('page', 0))
    except Exception:
        return 0


def feed(host):
    last = ''
    reported = last_reported()
    shown = reported if reported > 0 else ARGS.last_read
    if shown > 0:
        last = ' pse:lastRead="%d" pse:lastReadDate="2026-08-30T10:00:00Z"' % shown
    tmpl = '/opds/v1.2/books/0A1/pages/{pageNumber}?zero_based=true'
    if ARGS.max_width:
        tmpl += '&amp;maxWidth={maxWidth}'
    return ('''<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom"
      xmlns:opds="http://opds-spec.org/2010/catalog"
      xmlns:pse="http://vaemendis.net/opds-pse/ns">
  <id>urn:series:1</id>
  <title>Stub Comics</title>
  <entry>
    <title>Saga, Vol. 1</title>
    <id>urn:book:0A1</id>
    <author><name>Stub Server</name></author>
    <summary>A fixture volume served by native/tools/pse-stub-server.py.</summary>
    <link rel="http://opds-spec.org/acquisition" href="/opds/v1.2/books/0A1/file/saga-1.cbz"
          type="application/vnd.comicbook+zip"/>
    <link rel="http://vaemendis.net/opds-pse/stream" href="%s"
          type="image/png" pse:count="%d"%s/>
  </entry>
</feed>''' % (tmpl, ARGS.pages, last)).encode('utf-8')


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    served_pages = 0
    lock = threading.Lock()

    def log_message(self, fmt, *a):     # keep the console readable; the request log is the record
        pass

    def authorised(self):
        want = 'Basic ' + base64.b64encode(b'reader:hunter2').decode()
        return self.headers.get('Authorization') == want

    def note(self, verb):
        # The request log records WHETHER an Authorization header arrived, never its value.
        log('requests.log', '%s %s auth=%s' % (verb, self.path,
                                               'yes' if self.headers.get('Authorization') else 'NO'))

    def deny(self):
        self.send_response(401)
        self.send_header('WWW-Authenticate', 'Basic realm="stub"')
        self.send_header('Content-Length', '0')
        self.end_headers()

    def body(self, data, ctype):
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.note('GET')
        if not self.authorised():
            self.deny()
            return
        path = self.path.split('?')[0]
        if path == '/opds/v1.2/series/1':
            self.body(feed(self.headers.get('Host', '')), 'application/atom+xml;profile=opds-catalog')
            return
        if path == '/opds/v1.2/books/0A1/file/saga-1.cbz':
            self.body(cbz(ARGS.pages), 'application/vnd.comicbook+zip')
            return
        if path.startswith('/opds/v1.2/books/0A1/pages/'):
            n = path.rsplit('/', 1)[-1]
            if not n.isdigit():
                self.send_error(404)
                return
            with Handler.lock:
                Handler.served_pages += 1
                served = Handler.served_pages
            if ARGS.die_after and served > ARGS.die_after:
                log('requests.log', 'STALL page %s (served %d already)' % (n, served - 1))
                time.sleep(600)      # past any client watchdog: the stalled-stream case
                return
            self.body(png(600, 900, int(n) + 1, ARGS.pages), 'image/png')
            return
        self.send_error(404)

    def do_PATCH(self):
        self.note('PATCH')
        if not self.authorised():
            self.deny()
            return
        length = int(self.headers.get('Content-Length', '0'))
        payload = self.rfile.read(length).decode('utf-8', 'replace')
        if self.path.split('?')[0] == '/api/v1/books/0A1/read-progress':
            log('progress.log', payload)
            self.send_response(204)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        self.send_error(404)

    def do_POST(self):
        self.do_PATCH()


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    # NOT allow_reuse_address. On Windows SO_REUSEADDR lets a SECOND process bind a port that is
    # already listening, and the two then split the incoming connections between them - so a stub
    # left running from an earlier drive silently swallows some of the page requests and the recorded
    # fetch order comes out with holes in it. Refusing to bind is the honest failure.
    allow_reuse_address = False


def main():
    global ARGS, STATE
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=8975)
    ap.add_argument('--pages', type=int, default=12)
    ap.add_argument('--last-read', type=int, default=7)
    ap.add_argument('--max-width', action='store_true')
    ap.add_argument('--die-after', type=int, default=0)
    ap.add_argument('--state', default=os.path.dirname(os.path.abspath(__file__)))
    ARGS = ap.parse_args()
    STATE = ARGS.state
    os.makedirs(STATE, exist_ok=True)
    print('pse-stub: http://127.0.0.1:%d/opds/v1.2/series/1  (reader / hunter2)  pages=%d lastRead=%d'
          % (ARGS.port, ARGS.pages, ARGS.last_read))
    sys.stdout.flush()
    Server(('127.0.0.1', ARGS.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
