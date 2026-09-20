// Headless check of the pure remote-control API core (src/core/RemoteApi.{h,cpp}) — issue #76.
//
// What this pins, without a socket or a window:
//   * parseRequest splits method / path / query / body off a raw HTTP/1.1 request and honours Content-Length
//     (a body shorter than declared is returned incomplete; a longer one is truncated; a malformed request
//     line is flagged invalid rather than crashing);
//   * route maps every /player action and /input direction onto the right Command; GET /state -> State; an
//     unknown path -> NotFound; a known path with the wrong method or a missing/bad parameter -> BadRequest;
//   * stateJson round-trips a known state (parsed back with Qt's own JSON reader, an independent oracle);
//   * httpResponse builds a well-formed status line + Content-Type + a Content-Length equal to the body size.
//
// Every expected value is hand-authored or computed independently of RemoteApi (raw request byte strings I
// wrote; a Content-Length I counted; a body size Qt reports) — never by round-tripping through the function
// under test in a way that would make the fixture a fixed point of it.
//
// Prints REMOTEAPI-OK on success; any failure prints REMOTEAPI-FAIL <cond> (line) and exits non-zero.
#include "RemoteApi.h"
#include "PlayOnDevice.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace RemoteApi;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "REMOTEAPI-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main()
{
    // ---- 1. parseRequest: request line, path, query, body, Content-Length ----------------------------------

    // A GET with a query string and no body. CRLF wire form.
    {
        const Request r = parseRequest("GET /state?foo=bar&n=7 HTTP/1.1\r\nHost: x\r\n\r\n");
        CHECK(r.valid);
        CHECK(r.method == Method::Get);
        CHECK(r.methodRaw == QStringLiteral("GET"));
        CHECK(r.path == QStringLiteral("/state"));
        CHECK(r.query.value(QStringLiteral("foo")) == QStringLiteral("bar"));
        CHECK(r.query.value(QStringLiteral("n")) == QStringLiteral("7"));
        CHECK(r.body.isEmpty());
        CHECK(r.bodyComplete);
    }

    // A POST with a JSON body whose byte length is declared correctly. Body is 21 bytes: {"action":"seek"} is
    // 17 chars; use a body I counted by hand.
    {
        const QByteArray jbody = "{\"action\":\"play\"}";   // 17 bytes (hand-counted)
        CHECK(jbody.size() == 17);
        const QByteArray raw = "POST /player HTTP/1.1\r\nContent-Length: 17\r\nContent-Type: application/json\r\n\r\n" + jbody;
        const Request r = parseRequest(raw);
        CHECK(r.valid);
        CHECK(r.method == Method::Post);
        CHECK(r.path == QStringLiteral("/player"));
        CHECK(r.body == jbody);
        CHECK(r.bodyComplete);
    }

    // Body SHORTER than the declared Content-Length: keep what arrived, flag it incomplete.
    {
        const QByteArray raw = "POST /player HTTP/1.1\r\nContent-Length: 50\r\n\r\n{\"action\":\"play\"}";
        const Request r = parseRequest(raw);
        CHECK(r.valid);
        CHECK(!r.bodyComplete);                        // 17 < 50
        CHECK(r.body == QByteArray("{\"action\":\"play\"}"));
    }

    // Body LONGER than the declared Content-Length: truncate to the declared length, and the rest is not body.
    {
        const QByteArray raw = "POST /player HTTP/1.1\r\nContent-Length: 4\r\n\r\nABCDEFGH";
        const Request r = parseRequest(raw);
        CHECK(r.valid);
        CHECK(r.bodyComplete);
        CHECK(r.body == QByteArray("ABCD"));           // exactly 4 bytes
    }

    // Bare-LF line endings (a lax hand client) still parse.
    {
        const Request r = parseRequest("GET /state HTTP/1.1\n\n");
        CHECK(r.valid);
        CHECK(r.path == QStringLiteral("/state"));
    }

    // Percent-encoded query is decoded ('+' -> space, %2F -> '/').
    {
        const Request r = parseRequest("GET /x?q=a+b%2Fc HTTP/1.1\r\n\r\n");
        CHECK(r.valid);
        CHECK(r.query.value(QStringLiteral("q")) == QStringLiteral("a b/c"));
    }

    // Malformed: a one-token request line is invalid (never a crash), and empty input is invalid.
    {
        const Request r = parseRequest("GET\r\n\r\n");
        CHECK(!r.valid);
    }
    {
        const Request r = parseRequest(QByteArray());
        CHECK(!r.valid);
    }

    // ---- 2. route: the decision table -----------------------------------------------------------------------

    auto post = [](const char* path, const QByteArray& json) {
        const QByteArray raw = QByteArray("POST ") + path + " HTTP/1.1\r\nContent-Length: "
            + QByteArray::number(json.size()) + "\r\n\r\n" + json;
        return parseRequest(raw);
    };
    auto get = [](const char* path) {
        return parseRequest(QByteArray("GET ") + path + " HTTP/1.1\r\n\r\n");
    };

    // GET /state -> State
    CHECK(route(get("/state")).kind == CommandKind::State);
    // /state with the wrong method -> BadRequest
    CHECK(route(post("/state", "{}")).kind == CommandKind::BadRequest);

    // Unknown path -> NotFound
    CHECK(route(get("/nope")).kind == CommandKind::NotFound);
    CHECK(route(get("/")).kind == CommandKind::NotFound);

    // An invalid request -> BadRequest
    {
        Request bad;   // valid == false by default
        CHECK(route(bad).kind == CommandKind::BadRequest);
    }

    // Every /player action maps to the right PlayerAction.
    {
        struct { const char* a; PlayerAction want; } cases[] = {
            { "play",      PlayerAction::Play },
            { "pause",     PlayerAction::Pause },
            { "playpause", PlayerAction::PlayPause },
            { "toggle",    PlayerAction::PlayPause },
            { "stop",      PlayerAction::Stop },
            { "next",      PlayerAction::Next },
            { "prev",      PlayerAction::Prev },
            { "previous",  PlayerAction::Prev },
            { "subtitle",  PlayerAction::SubtitleCycle },
            { "sub",       PlayerAction::SubtitleCycle },
            { "audio",     PlayerAction::AudioCycle },
        };
        for (const auto& tc : cases)
        {
            const QByteArray body = QByteArray("{\"action\":\"") + tc.a + "\"}";
            const Command c = route(post("/player", body));
            CHECK(c.kind == CommandKind::Player);
            CHECK(c.player == tc.want);
        }
    }

    // /player with the wrong method -> BadRequest
    CHECK(route(get("/player")).kind == CommandKind::BadRequest);
    // /player with no action / an unknown action -> BadRequest
    CHECK(route(post("/player", "{}")).kind == CommandKind::BadRequest);
    CHECK(route(post("/player", "{\"action\":\"frobnicate\"}")).kind == CommandKind::BadRequest);

    // seek absolute vs relative, and the missing-target / bad-number errors.
    {
        const Command c = route(post("/player", "{\"action\":\"seek\",\"pos\":123.5}"));
        CHECK(c.kind == CommandKind::Player);
        CHECK(c.player == PlayerAction::Seek);
        CHECK(!c.seekRelative);
        CHECK(c.seekSeconds == 123.5);
    }
    {
        const Command c = route(post("/player", "{\"action\":\"seek\",\"rel\":-10}"));
        CHECK(c.player == PlayerAction::Seek);
        CHECK(c.seekRelative);
        CHECK(c.seekSeconds == -10.0);
    }
    CHECK(route(post("/player", "{\"action\":\"seek\"}")).kind == CommandKind::BadRequest);        // no pos/rel
    CHECK(route(post("/player", "{\"action\":\"seek\",\"pos\":\"x\"}")).kind == CommandKind::BadRequest); // NaN

    // volume: present + numeric, clamped to 0..100; missing -> BadRequest.
    {
        const Command c = route(post("/player", "{\"action\":\"volume\",\"level\":40}"));
        CHECK(c.player == PlayerAction::Volume);
        CHECK(c.volume == 40);
    }
    {
        const Command c = route(post("/player", "{\"action\":\"volume\",\"level\":250}"));
        CHECK(c.volume == 100);                       // clamped high
    }
    {
        const Command c = route(post("/player", "{\"action\":\"volume\",\"level\":-5}"));
        CHECK(c.volume == 0);                         // clamped low
    }
    CHECK(route(post("/player", "{\"action\":\"volume\"}")).kind == CommandKind::BadRequest);      // no level

    // A parameter may ride the query string instead of the body (POST /player?action=play).
    {
        const Request r = parseRequest("POST /player?action=play HTTP/1.1\r\n\r\n");
        const Command c = route(r);
        CHECK(c.kind == CommandKind::Player);
        CHECK(c.player == PlayerAction::Play);
    }

    // Every /input direction maps to the right InputDir.
    {
        struct { const char* d; InputDir want; } cases[] = {
            { "up",     InputDir::Up },
            { "down",   InputDir::Down },
            { "left",   InputDir::Left },
            { "right",  InputDir::Right },
            { "select", InputDir::Select },
            { "ok",     InputDir::Select },
            { "enter",  InputDir::Select },
            { "back",   InputDir::Back },
        };
        for (const auto& tc : cases)
        {
            const QByteArray body = QByteArray("{\"dir\":\"") + tc.d + "\"}";
            const Command c = route(post("/input", body));
            CHECK(c.kind == CommandKind::Input);
            CHECK(c.input == tc.want);
        }
    }
    CHECK(route(get("/input")).kind == CommandKind::BadRequest);                                   // wrong method
    CHECK(route(post("/input", "{}")).kind == CommandKind::BadRequest);                            // no dir
    CHECK(route(post("/input", "{\"dir\":\"diagonal\"}")).kind == CommandKind::BadRequest);        // unknown dir

    // ---- 3. stateJson: round-trip through Qt's own reader (independent oracle) -------------------------------
    {
        PlayerStateView s;
        s.hasMedia    = true;
        s.playing     = true;
        s.title       = QStringLiteral("Donkey Kong");
        s.positionSec = 42.0;
        s.durationSec = 100.0;
        s.volume      = 80;
        s.screen      = QStringLiteral("player");

        const QByteArray json = stateJson(s);
        const QJsonObject o = QJsonDocument::fromJson(json).object();
        CHECK(o.value(QStringLiteral("hasMedia")).toBool() == true);
        CHECK(o.value(QStringLiteral("playing")).toBool() == true);
        CHECK(o.value(QStringLiteral("title")).toString() == QStringLiteral("Donkey Kong"));
        CHECK(o.value(QStringLiteral("position")).toDouble() == 42.0);
        CHECK(o.value(QStringLiteral("duration")).toDouble() == 100.0);
        CHECK(o.value(QStringLiteral("volume")).toInt() == 80);
        CHECK(o.value(QStringLiteral("screen")).toString() == QStringLiteral("player"));
    }

    // ---- 4. httpResponse: status line + Content-Length = body size ------------------------------------------
    {
        const QByteArray body = "{\"ok\":true}";        // 11 bytes (hand-counted)
        CHECK(body.size() == 11);
        const QByteArray resp = httpResponse(200, body, "application/json");
        CHECK(resp.startsWith("HTTP/1.1 200 OK\r\n"));
        CHECK(resp.contains("Content-Type: application/json\r\n"));
        CHECK(resp.contains("Content-Length: 11\r\n"));
        CHECK(resp.contains("Connection: close\r\n"));
        CHECK(resp.endsWith(body));
        // The body follows a blank line, and nothing but the body follows it.
        const int sep = resp.indexOf("\r\n\r\n");
        CHECK(sep > 0);
        CHECK(resp.mid(sep + 4) == body);
    }
    {
        const QByteArray resp = httpResponse(404, "no", "text/plain");
        CHECK(resp.startsWith("HTTP/1.1 404 Not Found\r\n"));
        CHECK(resp.contains("Content-Length: 2\r\n"));
    }
    {
        const QByteArray resp = httpResponse(400, QByteArray(), "text/plain");
        CHECK(resp.startsWith("HTTP/1.1 400 Bad Request\r\n"));
        CHECK(resp.contains("Content-Length: 0\r\n"));   // empty body -> length 0
    }
    CHECK(QByteArray(reasonPhrase(405)) == "Method Not Allowed");

    // ---- #115: the LAN file drop's routes ------------------------------------------------------------------
    {
        auto req = [](const QByteArray& raw) { return parseRequest(raw); };

        // GET /drop is the page, whatever the query; nothing about the request selects anything else.
        CHECK(route(req("GET /drop HTTP/1.1\r\n\r\n")).kind == CommandKind::DropPage);
        CHECK(route(req("GET /drop?file=../../etc/passwd HTTP/1.1\r\n\r\n")).kind == CommandKind::DropPage);
        CHECK(route(req("GET /drop?path=C:%5CWindows HTTP/1.1\r\n\r\n")).kind == CommandKind::DropPage);
        CHECK(route(req("POST /drop HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}")).kind == CommandKind::BadRequest);
        CHECK(route(req("PUT /drop HTTP/1.1\r\n\r\n")).kind == CommandKind::BadRequest);
        // A near-miss is not the page.
        CHECK(route(req("GET /drop/ HTTP/1.1\r\n\r\n")).kind == CommandKind::NotFound);
        CHECK(route(req("GET /drop/../state HTTP/1.1\r\n\r\n")).kind == CommandKind::NotFound);
        CHECK(route(req("GET /dropx HTTP/1.1\r\n\r\n")).kind == CommandKind::NotFound);
        CHECK(route(req("GET /drop/drop.html HTTP/1.1\r\n\r\n")).kind == CommandKind::NotFound);

        CHECK(route(req("GET /drop/destinations HTTP/1.1\r\n\r\n")).kind == CommandKind::DropDestinations);
        CHECK(route(req("POST /drop/destinations HTTP/1.1\r\n\r\n")).kind == CommandKind::BadRequest);

        const QByteArray startBody = "{\"dest\":\"d\",\"name\":\"A.sfc\",\"size\":10}";
        CHECK(route(req("POST /drop/start HTTP/1.1\r\nContent-Length: " + QByteArray::number(startBody.size())
                        + "\r\n\r\n" + startBody)).kind == CommandKind::DropStart);
        CHECK(route(req("POST /drop/start HTTP/1.1\r\n\r\n")).kind == CommandKind::BadRequest);   // no body
        CHECK(route(req("GET /drop/start HTTP/1.1\r\n\r\n")).kind == CommandKind::BadRequest);
        CHECK(route(req("POST /drop/finish HTTP/1.1\r\nContent-Length: 8\r\n\r\n{\"id\":1}")).kind == CommandKind::DropFinish);
        CHECK(route(req("GET /drop/finish HTTP/1.1\r\n\r\n")).kind == CommandKind::BadRequest);

        const Command st = route(req("GET /drop/status?id=abc HTTP/1.1\r\n\r\n"));
        CHECK(st.kind == CommandKind::DropStatus);
        CHECK(st.dropId == QLatin1String("abc"));
        CHECK(route(req("GET /drop/status HTTP/1.1\r\n\r\n")).kind == CommandKind::BadRequest);   // no id
        CHECK(route(req("POST /drop/status?id=abc HTTP/1.1\r\n\r\n")).kind == CommandKind::BadRequest);

        const Request put = req("PUT /drop/chunk?id=abc&offset=8388608 HTTP/1.1\r\nContent-Length: 5\r\n\r\n");
        CHECK(put.method == Method::Put);
        const Command ch = route(put);
        CHECK(ch.kind == CommandKind::DropChunk);
        CHECK(ch.dropId == QLatin1String("abc"));
        CHECK(ch.dropOffset == 8388608);
        CHECK(route(req("PUT /drop/chunk?id=abc HTTP/1.1\r\n\r\n")).kind == CommandKind::BadRequest);          // no offset
        CHECK(route(req("PUT /drop/chunk?id=abc&offset=-1 HTTP/1.1\r\n\r\n")).kind == CommandKind::BadRequest);
        CHECK(route(req("PUT /drop/chunk?id=abc&offset=x HTTP/1.1\r\n\r\n")).kind == CommandKind::BadRequest);
        CHECK(route(req("PUT /drop/chunk?offset=0 HTTP/1.1\r\n\r\n")).kind == CommandKind::BadRequest);        // no id
        CHECK(route(req("POST /drop/chunk?id=abc&offset=0 HTTP/1.1\r\n\r\n")).kind == CommandKind::BadRequest);
        // PUT means nothing to the older routes.
        CHECK(route(req("PUT /state HTTP/1.1\r\n\r\n")).kind == CommandKind::BadRequest);

        // Streaming: every PUT /drop/chunk streams, within the 8 MiB cap; nothing else changed plan.
        CHECK(bodyPlanFor(req("PUT /drop/chunk?id=a&offset=0 HTTP/1.1\r\nContent-Length: 8388608\r\n\r\n")) == BodyPlan::Stream);
        CHECK(bodyPlanFor(req("PUT /drop/chunk?id=a&offset=0 HTTP/1.1\r\nContent-Length: 1\r\n\r\n")) == BodyPlan::Stream);
        CHECK(bodyPlanFor(req("PUT /drop/chunk?id=a&offset=0 HTTP/1.1\r\nContent-Length: 8388609\r\n\r\n")) == BodyPlan::TooLarge);
        CHECK(bodyPlanFor(req("PUT /drop/chunk?id=a&offset=0 HTTP/1.1\r\n\r\n")) == BodyPlan::LengthRequired);
        CHECK(bodyPlanFor(req("POST /drop/start HTTP/1.1\r\nContent-Length: 10\r\n\r\n")) == BodyPlan::Buffer);
        CHECK(bodyPlanFor(req("POST /drop/chunk HTTP/1.1\r\nContent-Length: 10\r\n\r\n")) == BodyPlan::Buffer);
        CHECK(bodyPlanFor(req("GET /drop HTTP/1.1\r\n\r\n")) == BodyPlan::Buffer);
        CHECK(isDropChunk(req("PUT /drop/chunk?id=a HTTP/1.1\r\n\r\n")));
        CHECK(!isDropChunk(req("PUT /drop/chunks HTTP/1.1\r\n\r\n")));
        CHECK(kDropChunkStreamCap == 8LL * 1024 * 1024);
        CHECK(requestCapBytes("POST /drop/start HTTP/1.1\r\n") == kDefaultRequestCap);   // small bodies stay small

        // The token rule: every /drop route but the page.
        for (const char* r : { "/drop/destinations", "/drop/start", "/drop/status", "/drop/chunk", "/drop/finish",
                               "/drop/", "/drop/anything" })
            CHECK(PlayOn::routeNeedsToken(QString::fromLatin1(r)));
        CHECK(!PlayOn::routeNeedsToken(QStringLiteral("/drop")));

        // The page's extra headers, and a header value that would split the response is dropped.
        const QByteArray resp = httpResponse(200, "x", "text/html; charset=utf-8",
                                             { QByteArray("X-A: 1"), QByteArray("X-B: 2\r\nSet-Cookie: t=1") });
        CHECK(resp.contains("\r\nX-A: 1\r\n"));
        CHECK(!resp.contains("Set-Cookie"));
        CHECK(resp.endsWith("\r\n\r\nx"));
        CHECK(QByteArray(reasonPhrase(507)) == "Insufficient Storage");
    }

    // ---- 6. #423: the origin gate -------------------------------------------------------------------------
    //
    // One pure gate for the whole listener. Every case below is a request an attacker or a real browser can
    // actually put on the wire; the expected answers are hand-authored, never derived from the function under
    // test.
    {
        const QString self = PlayOn::advertisedHostName(QStringLiteral("a1b2c3d4e5f6"));
        CHECK(self == QLatin1String("a1b2c3d4e5f6.local"));

        // --- the Host rule: accepted ---
        for (const char* h : { "192.168.1.5:8080", "192.168.1.5", "127.0.0.1", "10.0.0.7:1", "8.8.8.8:65535",
                               "[::1]:8080", "[::1]", "[::]", "[2001:db8::1]:443", "[fe80::1%25eth0]:8080",
                               "[fe80::1%eth0]", "[::ffff:192.168.1.5]", "[1:2:3:4:5:6:7:8]",
                               "localhost", "localhost:8080", "LocalHost:8080", "LOCALHOST",
                               "a1b2c3d4e5f6.local", "A1B2C3D4E5F6.LOCAL", "a1b2c3d4e5f6.local:8080",
                               "a1b2c3d4e5f6.local.", "a1b2c3d4e5f6.local.:8080", "  127.0.0.1:8080  " })
            CHECK(hostAllowed(h, self));

        // --- the Host rule: refused. A name that merely CONTAINS or ENDS WITH an accepted one is another
        //     name; so is another device's .local; so is a Host carrying credentials, a path or a query. ---
        for (const char* h : { "evil.example.com", "evil.example.com:8080", "EVIL.EXAMPLE.COM",
                               "192.168.1.5.evil.com", "192.168.1.5.evil.com:8080",
                               "evilocalhost", "localhost.evil.com", "xlocalhost:8080", "localhostx",
                               "deadbeefcafe.local", "deadbeefcafe.local:8080",          // another device
                               "a1b2c3d4e5f6.local.evil.com", "evil-a1b2c3d4e5f6.local", "xa1b2c3d4e5f6.local",
                               "a1b2c3d4e5f6.locale", ".local", "local",
                               "", " ", "\t", ":8080", "192.168.1.5:", "192.168.1.5::8080",
                               "user:pass@192.168.1.5", "192.168.1.5@evil.com", "192.168.1.5/drop",
                               "192.168.1.5:8080/drop", "192.168.1.5?x=1", "192.168.1.5#f",
                               "192.168.1.5\\drop", "192.168.1.5 evil.com", "192.168.1.5,evil.com",
                               "192.168.1.5:0", "192.168.1.5:99999", "192.168.1.5:80x", "192.168.1.5:-1",
                               "010.0.0.1", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.4:8080:9",
                               "::1", "::1:8080", "[::1", "::1]", "[]", "[::1]:x", "[::1]x", "[1::2::3]",
                               "[1:2:3:4:5:6:7:8:9]", "[gggg::1]", "[fe80::1%]", "[12345::1]" })
            CHECK(!hostAllowed(h, self));

        // The .local name is accepted only when this device HAS one, and only its own.
        CHECK(!hostAllowed("a1b2c3d4e5f6.local", QString()));
        CHECK(!hostAllowed("a1b2c3d4e5f6.local", QStringLiteral(".local")));
        CHECK(!hostAllowed(".local", QStringLiteral(".local")));
        CHECK(!hostAllowed("device.lan", QStringLiteral("device.lan")));      // only a .local name is taken
        CHECK(hostAllowed("127.0.0.1", QString()));                            // IP literals never need one

        // --- parseRequest keeps the two headers, and counts them ---
        {
            const Request r = parseRequest("GET /drop HTTP/1.1\r\nHost: 192.168.1.5:8080\r\n"
                                           "Origin: http://192.168.1.5:8080\r\n\r\n");
            CHECK(r.host == "192.168.1.5:8080");
            CHECK(r.origin == "http://192.168.1.5:8080");
            CHECK(r.hostSeen == 1);
            CHECK(r.originSeen == 1);
            CHECK(requestAllowed(r, self));
            const Request bare = parseRequest("GET /drop HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
            CHECK(bare.originSeen == 0);
            CHECK(requestAllowed(bare, self));                                 // a same-origin fetch sends none
            const Request twice = parseRequest("GET /drop HTTP/1.1\r\nHost: 127.0.0.1\r\nHost: evil.example.com\r\n\r\n");
            CHECK(twice.hostSeen == 2);
            CHECK(!requestAllowed(twice, self));                               // two Hosts is malformed, not a choice
            const Request noHost = parseRequest("GET /drop HTTP/1.1\r\n\r\n");
            CHECK(noHost.hostSeen == 0);
            CHECK(!requestAllowed(noHost, self));                              // HTTP/1.1 without a Host
            const Request lower = parseRequest("GET /drop HTTP/1.1\r\nhost: 127.0.0.1\r\norigin: http://127.0.0.1\r\n\r\n");
            CHECK(lower.hostSeen == 1);                                        // header names are case-insensitive
            CHECK(lower.originSeen == 1);
            CHECK(requestAllowed(lower, self));
        }

        // --- the Origin rule ---
        auto withOrigin = [&](const char* host, const char* origin) {
            QByteArray raw = QByteArray("GET /drop HTTP/1.1\r\nHost: ") + host + "\r\n";
            if (origin) raw += QByteArray("Origin: ") + origin + "\r\n";
            return requestAllowed(parseRequest(raw + "\r\n"), self);
        };
        CHECK(withOrigin("192.168.1.5:8080", "http://192.168.1.5:8080"));      // matching
        CHECK(withOrigin("192.168.1.5:8080", "HTTP://192.168.1.5:8080"));      // the scheme is case-insensitive
        CHECK(withOrigin("192.168.1.5", "http://192.168.1.5"));                // both on the default port
        CHECK(withOrigin("localhost:8080", "http://localhost:8080"));
        CHECK(withOrigin("a1b2c3d4e5f6.local:8080", "http://a1b2c3d4e5f6.local:8080"));
        CHECK(withOrigin("[::1]:8080", "http://[::1]:8080"));
        CHECK(withOrigin("127.0.0.1:8080", nullptr));                          // absent stays allowed
        CHECK(!withOrigin("192.168.1.5:8080", "http://evil.example.com"));     // cross-origin
        CHECK(!withOrigin("192.168.1.5:8080", "https://evil.example.com"));
        CHECK(!withOrigin("192.168.1.5:8080", "http://192.168.1.5:9090"));     // same name, another port
        CHECK(!withOrigin("192.168.1.5:8080", "http://192.168.1.5"));          // ... including the default one
        CHECK(!withOrigin("192.168.1.5", "http://192.168.1.5:8080"));
        CHECK(!withOrigin("192.168.1.5:443", "https://192.168.1.5"));          // https is a different origin
        CHECK(!withOrigin("192.168.1.5:8080", "null"));                        // a sandboxed frame
        CHECK(!withOrigin("192.168.1.5:8080", "192.168.1.5:8080"));            // not a serialized origin
        CHECK(!withOrigin("192.168.1.5:8080", "http://192.168.1.5:8080/drop")); // an origin has no path
        CHECK(!withOrigin("192.168.1.5:8080", "file://"));
        CHECK(!withOrigin("192.168.1.5:8080", "chrome-extension://abcd"));
        CHECK(!withOrigin("192.168.1.5:8080", ""));
        CHECK(!withOrigin("evil.example.com", "http://evil.example.com"));     // an Origin cannot rescue a Host
        CHECK(!requestAllowed(parseRequest("GET /drop HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://127.0.0.1\r\n"
                                           "Origin: http://evil.example.com\r\n\r\n"), self));   // two Origins

        // --- the gate is the same for EVERY route on this listener, and it does not read the method's body ---
        for (const char* path : { "/drop", "/drop/destinations", "/drop/start", "/drop/status", "/drop/chunk",
                                  "/drop/finish", "/pair", "/state", "/player", "/input", "/open", "/inventory",
                                  "/bundle", "/gamelists", "/gamelists/flush", "/nonsense" })
        {
            const QByteArray target = QByteArray(path);
            CHECK(!requestAllowed(parseRequest("GET " + target + " HTTP/1.1\r\nHost: evil.example.com\r\n\r\n"), self));
            CHECK(requestAllowed(parseRequest("GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"), self));
            CHECK(!requestAllowed(parseRequest("OPTIONS " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"), self));
        }
        // ... including the one request that streams: its plan is never consulted for a refused Host.
        CHECK(!requestAllowed(parseRequest("PUT /drop/chunk?id=a&offset=0 HTTP/1.1\r\nHost: evil.example.com\r\n"
                                           "Content-Length: 4096\r\n\r\n"), self));
        CHECK(requestAllowed(parseRequest("PUT /drop/chunk?id=a&offset=0 HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                          "Content-Length: 4096\r\n\r\n"), self));
        // An unparseable request is answered 400 as it always was; the gate does not claim it.
        CHECK(requestAllowed(parseRequest("not-a-request"), self));
        CHECK(requestAllowed(Request(), self));

        // --- the refusal: 403, plain text, nothing about what is served here, and never a CORS header ---
        const QByteArray forbidden = forbiddenResponse();
        CHECK(forbidden.startsWith("HTTP/1.1 403 Forbidden\r\n"));
        CHECK(forbidden.contains("Content-Type: text/plain\r\n"));
        CHECK(forbidden.contains("Content-Length: 10\r\n"));
        CHECK(forbidden.endsWith("\r\n\r\nforbidden\n"));
        CHECK(forbidden.size() < 160);
        for (const char* leak : { "drop", "pair", "EverythingBox", "token", "Access-Control" })
            CHECK(!forbidden.contains(leak));
        CHECK(QByteArray(reasonPhrase(403)) == "Forbidden");
        // No response this surface can build carries one either.
        for (int status : { 200, 400, 401, 403, 404, 411, 413, 507 })
            CHECK(!httpResponse(status, "x", "application/json").contains("Access-Control"));
        CHECK(!httpResponse(200, "x", "text/html", { QByteArray("X-A: 1") }).contains("Access-Control"));
    }

    if (failures == 0) std::printf("REMOTEAPI-OK\n");
    else               std::fprintf(stderr, "REMOTEAPI had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
