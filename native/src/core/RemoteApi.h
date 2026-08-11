// A tiny, CONTROL-ONLY HTTP remote-control surface (issue #76): enough of an API to make any phone browser a
// play/pause/seek/navigate remote, and deliberately no more than that. There is no filesystem path, no eval,
// no library browsing — the whole vocabulary is the three routes below.
//
// This file (and RemoteApi.cpp) is the PURE half: raw request bytes in, a decoded Request out; a Request in,
// a routed Command out; a plain state struct in, the /state JSON out; a status + body in, the raw response
// bytes out. It pulls in QtCore only — no QTcpServer, no MpvWidget, no NavContext — so probe_remoteapi can
// drive every decision without a socket or a window, and mutate.py can pin each one. RemoteServer.{h,cpp}
// owns the live QTcpServer and dispatches a Command onto the running app; it is the only half that touches
// the network or the UI.
#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>

namespace RemoteApi
{
    enum class Method { Get, Post, Other };

    // A minimally-parsed HTTP/1.1 request: the request line (method + target), the Content-Length body, and
    // the decoded path/query. Not a full HTTP stack — no chunked bodies, no header semantics beyond
    // Content-Length — which is all the tiny surface here needs. Malformed input yields `valid == false`
    // (never a throw, never a crash); an unfinished body yields `bodyComplete == false` with whatever arrived.
    struct Request
    {
        bool    valid        = false;  // false => the bytes were not a parseable request line
        bool    bodyComplete = true;   // false => fewer bytes than Content-Length declared actually arrived
        Method  method       = Method::Other;
        QString methodRaw;             // "GET" / "POST" / ... exactly as sent
        QString path;                  // percent-decoded path, no query ("/player")
        QMap<QString, QString> query;  // percent-decoded key -> value from the target's query string
        QByteArray body;               // exactly Content-Length bytes when declared and present
    };

    // Split a raw request into method / path / query / body, honouring Content-Length (a body shorter than
    // declared is returned with bodyComplete=false; a longer one is truncated to the declared length). Any
    // failure to find a two-token request line returns an invalid Request.
    Request parseRequest(const QByteArray& rawHttp);

    enum class CommandKind { State, Player, Input, NotFound, BadRequest };

    // The /player verbs. PlayPause is the toggle a single remote button wants; Play/Pause force a state.
    enum class PlayerAction
    {
        None, PlayPause, Play, Pause, Stop, Seek, Next, Prev, Volume, SubtitleCycle, AudioCycle
    };

    // The /input directions — the D-pad. Select = Enter/OK, Back = the logical back action.
    enum class InputDir { None, Up, Down, Left, Right, Select, Back };

    struct Command
    {
        CommandKind  kind   = CommandKind::NotFound;
        PlayerAction player = PlayerAction::None;
        InputDir     input  = InputDir::None;
        // Seek target. Absolute (seek to this second) unless seekRelative, in which case a signed delta.
        bool    seekRelative = false;
        double  seekSeconds  = 0.0;
        int     volume       = -1;    // 0..100 on a Volume command; -1 otherwise
        QString error;                // human-readable reason on a BadRequest (never shown as HTML)
    };

    // The decision table. (method, path, body/query) -> a Command. A known path with the wrong method or a
    // missing/invalid parameter is a BadRequest (with a reason); an unknown path is a NotFound; an unparseable
    // request is a BadRequest. This is the whole of the routing contract and what the probe pins exhaustively.
    Command route(const Request& req);

    // A snapshot of what the app is doing, plain data so route()/stateJson() stay pure (the live MpvWidget is
    // read into one of these by RemoteServer's caller, never here).
    struct PlayerStateView
    {
        bool    hasMedia = false;      // is something loaded in the player at all
        bool    playing  = false;      // playing (true) vs paused (false)
        QString title;                 // now-playing title ("" when nothing is playing)
        double  positionSec = 0.0;
        double  durationSec = 0.0;
        int     volume   = 0;          // 0..100 UI level
        QString screen;                // "home" | "player" | "browse" | ... the current app screen
    };

    // Serialize a state view to the compact /state JSON body.
    QByteArray stateJson(const PlayerStateView& s);

    // Build a complete HTTP/1.1 response: status line + Content-Type + a Content-Length computed from the body
    // + Connection: close + the body. status is one of 200/400/404/405; contentType e.g. "application/json".
    QByteArray httpResponse(int status, const QByteArray& body, const char* contentType);

    // The reason phrase for a status code ("OK", "Bad Request", ...). Exposed so the probe can pin the status
    // line without re-deriving the table.
    const char* reasonPhrase(int status);
}
