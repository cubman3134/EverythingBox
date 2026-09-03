#!/usr/bin/env python3
"""A fixture AniList stub (issue #156), for driving the tracker against the real app with no account.

WHY THIS EXISTS. probe_tracker pins every rule with no I/O, which is the right place for rules -- but it
cannot say whether the APP reaches the network at all, whether the OAuth loopback completes, whether the
match prompt appears, or whether a mutation really leaves the box. Those need a server, and the real AniList
is not one a gate may depend on: it needs somebody's account, it rate-limits, and a push against it edits a
real list. So this stands in for it.

It speaks the three shapes AniListTracker sends and NOTHING else, deliberately: a stub that tolerated a
malformed request would hide the bug where the app sends one.

    GET  /api/v2/oauth/authorize   -> 302 back to `redirect_uri` carrying ?code=FIXTURE-CODE
    POST /api/v2/oauth/token       -> {"access_token": ..., "refresh_token": ..., "expires_in": ...}
    POST /graphql                  -> routed on the operation in the body:
                                        Page(...)            -> two search matches
                                        Media(id: ...)       -> the account's entry, at whatever progress
                                                                /__progress was last set to
                                        SaveMediaListEntry   -> accepts and RECORDS the mutation
    GET  /__log                    -> everything received, as JSON (what a drive asserts against)
    GET  /__progress?n=N           -> set the account's progress, to make the stub AHEAD of the app
    GET  /__reset                  -> forget the log

Point the app at it with:
    EB_ANILIST_ENDPOINT=http://127.0.0.1:8799/graphql
    EB_ANILIST_AUTH=http://127.0.0.1:8799/api/v2/oauth

NOTHING IN THE LOG IS A CREDENTIAL and nothing here prints one: the token bodies are recorded by their
grant_type and their field NAMES only, never their values, because a drive transcript is something a person
pastes into a report.
"""
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, urlencode

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8799

STATE = {'progress': 0, 'status': 'CURRENT', 'log': []}
LOCK = threading.Lock()

# The two matches every search answers with. Chosen so one is unambiguously manga (a chapter count, no
# episode count) and one unambiguously anime, which is what the app's kind mapping reads.
MATCHES = [
    {"id": 30002, "title": {"romaji": "Berserk", "english": None},
     "startDate": {"year": 1989}, "episodes": None, "chapters": 364,
     "coverImage": {"large": "http://127.0.0.1/cover1.jpg"}},
    {"id": 20605, "title": {"romaji": "Boku no Hero", "english": "My Hero Academia"},
     "startDate": {"year": 2016}, "episodes": 13, "chapters": None,
     "coverImage": {"large": "http://127.0.0.1/cover2.jpg"}},
]


def note(kind, detail):
    with LOCK:
        STATE['log'].append({'op': kind, 'detail': detail})
        print('[stub] %s %s' % (kind, json.dumps(detail)), flush=True)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass   # the transcript is ours, not http.server's

    def _send(self, code, body, ctype='application/json', extra=None):
        raw = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(raw)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path == '/api/v2/oauth/authorize':
            redirect = (q.get('redirect_uri') or [''])[0]
            # The client id is public by design and is recorded; there is no secret in an authorize URL.
            note('authorize', {'client_id': (q.get('client_id') or [''])[0],
                               'redirect_uri': redirect,
                               'response_type': (q.get('response_type') or [''])[0]})
            target = redirect + '?' + urlencode({'code': 'FIXTURE-CODE'})
            self._send(302, b'', 'text/plain', {'Location': target})
            return
        if u.path == '/__log':
            with LOCK:
                self._send(200, STATE['log'])
            return
        if u.path == '/__progress':
            with LOCK:
                STATE['progress'] = int((q.get('n') or ['0'])[0])
                if q.get('status'):
                    STATE['status'] = q['status'][0]
                self._send(200, {'progress': STATE['progress'], 'status': STATE['status']})
            return
        if u.path == '/__reset':
            with LOCK:
                STATE['log'] = []
            self._send(200, {'ok': True})
            return
        self._send(404, {'error': 'no such path'})

    def do_POST(self):
        u = urlparse(self.path)
        n = int(self.headers.get('Content-Length') or 0)
        raw = self.rfile.read(n)
        try:
            body = json.loads(raw)
        except Exception:
            note('malformed', {'path': u.path, 'bytes': n})
            self._send(400, {'error': 'not json'})
            return

        if u.path == '/api/v2/oauth/token':
            # FIELD NAMES ONLY. The values here are a client secret and an authorization code.
            note('token', {'grant_type': body.get('grant_type'), 'fields': sorted(body.keys())})
            self._send(200, {'token_type': 'Bearer', 'expires_in': 31536000,
                             'access_token': 'FIXTURE-ACCESS', 'refresh_token': 'FIXTURE-REFRESH'})
            return

        if u.path.rstrip('/') in ('/graphql', ''):
            query = body.get('query') or ''
            variables = body.get('variables') or {}
            authed = self.headers.get('Authorization', '').startswith('Bearer ')
            if 'SaveMediaListEntry' in query:
                # RECORD it. This is the assertion a drive is really making: the app got as far as sending a
                # mutation, and it sent THESE variables.
                note('save', {'authed': authed, 'variables': variables})
                with LOCK:
                    STATE['progress'] = int(variables.get('progress') or 0)
                    STATE['status'] = variables.get('status') or STATE['status']
                self._send(200, {'data': {'SaveMediaListEntry': {
                    'id': 5551, 'progress': STATE['progress'], 'status': STATE['status']}}})
                return
            if 'Page(' in query:
                note('search', {'authed': authed, 'variables': variables})
                self._send(200, {'data': {'Page': {'media': MATCHES}}})
                return
            if 'Media(id' in query:
                with LOCK:
                    p, st = STATE['progress'], STATE['status']
                note('entry', {'authed': authed, 'variables': variables, 'answering_progress': p})
                self._send(200, {'data': {'Media': {
                    'id': int(variables.get('mediaId') or 0), 'episodes': None, 'chapters': 364,
                    'mediaListEntry': {'id': 5551, 'progress': p, 'status': st, 'score': 0}}}})
                return
            note('unknown-operation', {'first80': query[:80]})
            self._send(200, {'errors': [{'message': 'unknown operation'}], 'data': None})
            return

        self._send(404, {'error': 'no such path'})


if __name__ == '__main__':
    print('[stub] AniList fixture on http://127.0.0.1:%d' % PORT, flush=True)
    # THREADING, not the plain HTTPServer: the app holds a keep-alive connection open while a
    # drive is also asking /__log, and a single-threaded server serialises those into a hang that
    # looks exactly like the app never sending the request.
    ThreadingHTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
