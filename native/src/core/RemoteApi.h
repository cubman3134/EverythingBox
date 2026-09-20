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
#include <QList>
#include <QMap>
#include <QString>

namespace RemoteApi
{
    enum class Method { Get, Post, Put, Other };   // Put: #115's /drop/chunk, and nothing else

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
        // The bearer credential a PAIRED peer presents (issue #143), read off `Authorization: Bearer <t>` or
        // `X-EB-Token: <t>`. A CREDENTIAL: it is compared and then dropped -- never logged, never echoed into
        // a response body, never written to a transcript.
        QString token;
        // Issue #291. The declared Content-Length as a 64-bit number (-1 when absent or unparseable) and the
        // lower-cased media type with any parameters stripped. The streaming decision (bodyPlanFor) reads
        // these off the HEADERS alone, before a body byte is accepted.
        qint64     declaredLength = -1;
        QByteArray contentType;
        // Issue #423. The Host and Origin headers, kept for the origin gate below. The values exactly as
        // sent -- the gate does its own trimming and case folding -- plus how many times each header
        // appeared, so a request carrying two Hosts (malformed, and the shape a smuggled second one arrives
        // in) can be refused outright rather than quietly answering for the last of them.
        QByteArray host;
        QByteArray origin;
        int hostSeen   = 0;
        int originSeen = 0;
    };

    // Split a raw request into method / path / query / body, honouring Content-Length (a body shorter than
    // declared is returned with bodyComplete=false; a longer one is truncated to the declared length). Any
    // failure to find a two-token request line returns an invalid Request.
    Request parseRequest(const QByteArray& rawHttp);

    // State/Player/Input are #76's original three. Open/PairBegin/PairRedeem are #143's hand-off surface:
    // /open takes an item REFERENCE plus a position (never bytes) and /pair is how a source obtains the token
    // /open then requires. The BODY of an /open is left in Request::body and decoded by PlayOn::parseHandoff,
    // so this unit stays free of the hand-off vocabulary and the routing table can be tested apart from it.
    // Inventory/Bundle are #127's library-transfer surface, on the SAME listener and the same credential:
    // GET /inventory answers with what this device's art cache holds (item id + content stamp), POST /bundle
    // carries ONE item's art. Their bodies, like /open's, are left in Request::body and decoded by
    // LibraryBundle, so this unit never learns the bundle vocabulary either.
    // Gamelists is #292's GET /gamelists: per system folder under the ROM root, the ROM files present and the
    // games its gamelist already lists. Credentialled like /inventory; a gamelist entry itself rides POST
    // /bundle as a v2 body of its own kind.
    // GamelistFlush is #401's POST /gamelists/flush: the source saying one system's gamelist entries are all
    // sent, so the target commits that system's batch now. Credentialled like /gamelists; its small JSON body is
    // left in Request::body and decoded by LibraryBundle.
    // Drop* are #115's LAN file drop: GET /drop is the one self-contained page (never anything else, whatever
    // the query says); every other /drop route is credentialled (PlayOn::routeNeedsToken) and answered by
    // FileDrop. /drop/chunk is a PUT whose body is streamed, never buffered (bodyPlanFor).
    enum class CommandKind { State, Player, Input, Open, PairBegin, PairRedeem,
                             Inventory, Bundle, NotFound, BadRequest, Gamelists, GamelistFlush,
                             DropPage, DropDestinations, DropStart, DropStatus, DropChunk, DropFinish };

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
        QString pairCode;             // the code entered on a PairRedeem; empty on every other kind
        // #115: the upload a DropStatus / DropChunk names, and a DropChunk's offset (-1 when absent). The
        // DropStart / DropFinish JSON bodies stay in Request::body for the route to decode.
        QString dropId;
        qint64  dropOffset = -1;
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
        // What is playing, as a REFERENCE (issue #143) -- the same vocabulary /open accepts, so a peer's
        // /state can be read straight back into the open this device should perform ("Continue on this
        // device"). An empty kind/id means "playing something it cannot name", which is a refusal, not a
        // guess: opening by title would open the wrong thing.
        QString refKind;               // "catalog" | "addon" | "server" | "local"
        QString refId;
        QString refType;               // "movie" | "episode" | "music" | ...
        QString refTitle;
        QString refSource;             // addon / server key when the kind needs one
        QString audioTrack;            // selected track ids, opaque strings ("" = the source's default)
        QString subtitleTrack;
        bool    volumeControllable = false;   // does this device own a volume a remote may move
    };

    // Serialize a state view to the compact /state JSON body.
    QByteArray stateJson(const PlayerStateView& s);

    // Build a complete HTTP/1.1 response: status line + Content-Type + a Content-Length computed from the body
    // + Connection: close + the body. status is one of 200/400/404/405; contentType e.g. "application/json".
    QByteArray httpResponse(int status, const QByteArray& body, const char* contentType);
    // The same, with extra header lines (each "Name: value", no CRLF) before the blank line. #115's page uses
    // it for its Content-Security-Policy.
    QByteArray httpResponse(int status, const QByteArray& body, const char* contentType,
                            const QList<QByteArray>& extraHeaders);

    // The reason phrase for a status code ("OK", "Bad Request", ...). Exposed so the probe can pin the status
    // line without re-deriving the table.
    const char* reasonPhrase(int status);

    // How many bytes of ONE request this surface will buffer. #76's routes are tiny and keep a tiny cap — a
    // control API that buffers megabytes is a way to make the app eat memory. #127's POST /bundle is the one
    // route that legitimately carries a payload, so it (and only it) gets a payload-sized cap, decided HERE
    // from the request line rather than in the socket code, so the exception is one testable function rather
    // than a condition buried in a read loop. `rawPrefix` may be a partial request — whatever has arrived.
    constexpr int kDefaultRequestCap = 64 * 1024;
    constexpr int kBundleRequestCap  = 20 * 1024 * 1024;
    int requestCapBytes(const QByteArray& rawPrefix);

    // Issue #291: the ONE request this surface does not buffer. A POST /bundle whose Content-Type is the
    // raw-body bundle format is streamed to a spool file under the cache root instead, so its ceiling is not
    // a memory figure. Decided from the parsed HEADERS, once they are in and before any body is accepted:
    //   Buffer         -- every other request, exactly as before (requestCapBytes applies);
    //   Stream         -- POST /bundle, raw-body type, a declared length within kBundleStreamCap;
    //   TooLarge       -- the same, declaring more than kBundleStreamCap (413 without reading the body);
    //   LengthRequired -- the same with no usable Content-Length (411: a stream needs to know where it ends).
    // The server checks the paired token BEFORE acting on any of the three non-Buffer answers.
    constexpr qint64 kBundleStreamCap = 65LL * 1024 * 1024;
    constexpr const char* kBundleStreamContentType = "application/x-eb-bundle";
    //
    // Issue #115 adds the second streamed request: PUT /drop/chunk, any content type, a declared length within
    // kDropChunkStreamCap (FileDrop's 8 MiB piece; the header block is bounded separately by
    // kDefaultRequestCap in the header phase). Its body goes straight into the upload's part file.
    constexpr qint64 kDropChunkStreamCap = 8LL * 1024 * 1024;
    enum class BodyPlan { Buffer, Stream, TooLarge, LengthRequired };
    BodyPlan bodyPlanFor(const Request& headers);
    // True for the headers of a streamed file-drop piece (PUT /drop/chunk), whatever its plan says.
    bool isDropChunk(const Request& headers);

    // ---- Issue #423: the origin gate ---------------------------------------------------------------------
    //
    // This listener sits on a LAN address, and a browser will send a request to it on behalf of ANY page: a
    // hostile site can point a name it controls at this device's address (DNS rebinding) and then load /drop
    // from that name. It cannot upload -- every /drop/ route needs the code shown on this device's own screen
    // -- but what it renders IS this device's page, and a page can ask the user to read that code out loud.
    // So: ONE gate, pure, applied to EVERY route (the page, /pair, /state, /player, /input, /open,
    // /inventory, /bundle, /gamelists, all of /drop) BEFORE routing, before the token check, before a body
    // byte is accepted and before any spool or part file exists. A refused request never touches disk.
    //
    //   Host    -- an IP literal (IPv4, or IPv6 in brackets) with an optional port; `localhost` with an
    //              optional port; or THIS device's own advertised mDNS name (PlayOn::advertisedHostName,
    //              i.e. "<deviceId>.local"). Every other DNS name is refused, and so is a missing, empty,
    //              repeated or malformed Host, and one carrying credentials, a path, a query or whitespace.
    //              Matching is case-insensitive and EXACT once the port (and one trailing dot) is stripped:
    //              a name that merely ENDS WITH an accepted one ("192.168.1.5.evil.com", "evilocalhost") is a
    //              different name and is refused.
    //   Origin  -- when present, it must be an http origin (this listener is plaintext; no https page was
    //              ever loaded from it), its host must pass the same rule, and it must name the same origin
    //              as the request's own Host -- same name, same effective port, the default being 80 for
    //              both. A same-origin fetch may send no Origin at all, which stays allowed; "null",
    //              "file://", an extension scheme and every cross-origin value are refused.
    //   OPTIONS -- refused outright. Nothing here answers a CORS preflight, and NO response anywhere on this
    //              listener carries an Access-Control-* header, so a browser cannot read these answers
    //              cross-origin either.
    //
    // `selfLocalName` is this device's own ".local" name; empty (or anything not ending in ".local") means no
    // DNS name is accepted at all -- IP literals and localhost still are, so a device that advertises nothing
    // is still reachable and still not rebindable.
    bool hostAllowed(const QByteArray& hostHeaderValue, const QString& selfLocalName);
    bool requestAllowed(const Request& req, const QString& selfLocalName);

    // What a refused request is answered with: 403, a short plain-text body, and not one word about what is
    // served here.
    QByteArray forbiddenResponse();
}
