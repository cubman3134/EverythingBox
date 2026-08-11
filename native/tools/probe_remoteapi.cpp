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

    if (failures == 0) std::printf("REMOTEAPI-OK\n");
    else               std::fprintf(stderr, "REMOTEAPI had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
