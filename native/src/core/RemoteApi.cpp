#include "RemoteApi.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>

namespace RemoteApi
{
    namespace
    {
        // Strip one trailing '\r' so a header block split on '\n' is CRLF- and LF-clean either way (the wire is
        // CRLF; a hand-typed or Unix client may send bare LF).
        QByteArray stripCr(QByteArray line)
        {
            if (line.endsWith('\r')) line.chop(1);
            return line;
        }

        Method methodFrom(const QString& raw)
        {
            if (raw == QStringLiteral("GET"))  return Method::Get;
            if (raw == QStringLiteral("POST")) return Method::Post;
            return Method::Other;
        }

        // A parameter lookup that reads the query string first, then the JSON body. Query wins so a
        // `?action=play` is honoured even on a POST with an empty body. Returns a null QString when neither
        // carries the key, which the callers use to tell "absent" from "present but empty".
        QString param(const Request& req, const QJsonObject& body, const char* key)
        {
            const QString k = QString::fromLatin1(key);
            auto it = req.query.find(k);
            if (it != req.query.end()) return it.value();
            if (body.contains(k))
            {
                const QJsonValue v = body.value(k);
                if (v.isString()) return v.toString();
                if (v.isDouble()) return QString::number(v.toDouble());
                if (v.isBool())   return v.toBool() ? QStringLiteral("1") : QStringLiteral("0");
            }
            return QString();  // null: absent
        }
    }

    Request parseRequest(const QByteArray& raw)
    {
        Request req;
        if (raw.isEmpty()) return req;  // nothing to parse -> invalid

        // Split the header block from the body at the blank line. Accept CRLFCRLF (the wire) or LFLF (a lax
        // client). No separator at all means a request line with no body, which is fine for GET.
        int sepAt = raw.indexOf("\r\n\r\n");
        int sepLen = 4;
        if (sepAt < 0) { sepAt = raw.indexOf("\n\n"); sepLen = 2; }
        const QByteArray headerBlock = (sepAt < 0) ? raw : raw.left(sepAt);
        const QByteArray afterHeaders = (sepAt < 0) ? QByteArray() : raw.mid(sepAt + sepLen);

        QList<QByteArray> lines = headerBlock.split('\n');
        if (lines.isEmpty()) return req;

        // --- Request line: METHOD SP TARGET [SP VERSION] ---
        const QByteArray requestLine = stripCr(lines.first());
        QList<QByteArray> tok;
        for (const QByteArray& t : requestLine.split(' '))
            if (!t.isEmpty()) tok << t;
        if (tok.size() < 2) return req;  // need at least a method and a target -> malformed

        req.methodRaw = QString::fromLatin1(tok[0]);
        req.method    = methodFrom(req.methodRaw);

        // --- Target -> path + query ---
        const QByteArray target = tok[1];
        const int q = target.indexOf('?');
        const QByteArray pathRaw  = (q < 0) ? target : target.left(q);
        const QByteArray queryRaw = (q < 0) ? QByteArray() : target.mid(q + 1);
        req.path = QString::fromUtf8(QByteArray::fromPercentEncoding(pathRaw));

        if (!queryRaw.isEmpty())
            for (const QByteArray& pair : queryRaw.split('&'))
            {
                if (pair.isEmpty()) continue;
                const int eq = pair.indexOf('=');
                const QByteArray k = (eq < 0) ? pair : pair.left(eq);
                const QByteArray v = (eq < 0) ? QByteArray() : pair.mid(eq + 1);
                // '+' is a space in a query component; fromPercentEncoding does not do that itself.
                const QString key = QString::fromUtf8(QByteArray::fromPercentEncoding(QByteArray(k).replace('+', ' ')));
                const QString val = QString::fromUtf8(QByteArray::fromPercentEncoding(QByteArray(v).replace('+', ' ')));
                if (!key.isEmpty()) req.query.insert(key, val);
            }

        // --- Headers we care about: Content-Length, and the paired-device credential (#143) ---
        int contentLength = -1;
        for (int i = 1; i < lines.size(); ++i)
        {
            const QByteArray line = stripCr(lines[i]);
            const int colon = line.indexOf(':');
            if (colon < 0) continue;
            const QByteArray name = line.left(colon).trimmed().toLower();
            if (name == "content-length")
            {
                bool ok = false;
                const int n = line.mid(colon + 1).trimmed().toInt(&ok);
                if (ok && n >= 0) contentLength = n;
            }
            // The token a paired peer presents. Two spellings because a hand-written client reaches for
            // Authorization and a fetch() from the phone page reaches for a custom header; both mean the same
            // thing. Stored on the Request and NEVER logged from here or anywhere downstream.
            else if (name == "authorization")
            {
                const QByteArray v = line.mid(colon + 1).trimmed();
                if (v.toLower().startsWith("bearer "))
                    req.token = QString::fromLatin1(v.mid(7).trimmed());
            }
            else if (name == "x-eb-token")
            {
                req.token = QString::fromLatin1(line.mid(colon + 1).trimmed());
            }
        }

        // --- Body. A declared length shorter than what arrived truncates; longer marks it incomplete. ---
        if (contentLength >= 0)
        {
            if (afterHeaders.size() >= contentLength) { req.body = afterHeaders.left(contentLength); req.bodyComplete = true; }
            else                                      { req.body = afterHeaders;                     req.bodyComplete = false; }
        }
        else
        {
            req.body = afterHeaders;
        }

        req.valid = true;
        return req;
    }

    Command route(const Request& req)
    {
        Command c;
        if (!req.valid)
        {
            c.kind = CommandKind::BadRequest;
            c.error = QStringLiteral("malformed request");
            return c;
        }

        // The body, read as a JSON object when it is one (an empty or non-JSON body just yields no fields, and
        // the request can still carry its parameters in the query string).
        QJsonObject body;
        if (!req.body.isEmpty())
        {
            const QJsonDocument doc = QJsonDocument::fromJson(req.body);
            if (doc.isObject()) body = doc.object();
        }

        if (req.path == QStringLiteral("/state"))
        {
            if (req.method != Method::Get)
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/state is GET only");
                return c;
            }
            c.kind = CommandKind::State;
            return c;
        }

        if (req.path == QStringLiteral("/player"))
        {
            if (req.method != Method::Post)
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/player is POST only");
                return c;
            }
            c.kind = CommandKind::Player;
            const QString action = param(req, body, "action").toLower();
            if (action == QStringLiteral("play"))                                      c.player = PlayerAction::Play;
            else if (action == QStringLiteral("pause"))                                c.player = PlayerAction::Pause;
            else if (action == QStringLiteral("playpause") || action == QStringLiteral("toggle"))
                                                                                       c.player = PlayerAction::PlayPause;
            else if (action == QStringLiteral("stop"))                                 c.player = PlayerAction::Stop;
            else if (action == QStringLiteral("next"))                                 c.player = PlayerAction::Next;
            else if (action == QStringLiteral("prev") || action == QStringLiteral("previous"))
                                                                                       c.player = PlayerAction::Prev;
            else if (action == QStringLiteral("subtitle") || action == QStringLiteral("sub"))
                                                                                       c.player = PlayerAction::SubtitleCycle;
            else if (action == QStringLiteral("audio"))                                c.player = PlayerAction::AudioCycle;
            else if (action == QStringLiteral("seek"))
            {
                // Absolute "pos" wins; else a signed relative "rel". Neither present is a bad request — a seek
                // with no target is meaningless, and silently doing nothing would read as a broken remote.
                const QString pos = param(req, body, "pos");
                const QString rel = param(req, body, "rel");
                bool ok = false;
                if (!pos.isNull())
                {
                    const double v = pos.toDouble(&ok);
                    if (!ok) { c.kind = CommandKind::BadRequest; c.error = QStringLiteral("seek pos not a number"); return c; }
                    c.player = PlayerAction::Seek; c.seekRelative = false; c.seekSeconds = v;
                }
                else if (!rel.isNull())
                {
                    const double v = rel.toDouble(&ok);
                    if (!ok) { c.kind = CommandKind::BadRequest; c.error = QStringLiteral("seek rel not a number"); return c; }
                    c.player = PlayerAction::Seek; c.seekRelative = true; c.seekSeconds = v;
                }
                else
                {
                    c.kind = CommandKind::BadRequest; c.error = QStringLiteral("seek needs pos or rel"); return c;
                }
            }
            else if (action == QStringLiteral("volume"))
            {
                const QString level = param(req, body, "level");
                bool ok = false;
                const int v = level.toInt(&ok);
                if (level.isNull() || !ok)
                {
                    c.kind = CommandKind::BadRequest; c.error = QStringLiteral("volume needs a numeric level"); return c;
                }
                c.player = PlayerAction::Volume;
                c.volume = v < 0 ? 0 : (v > 100 ? 100 : v);   // clamp to the 0..100 UI range
            }
            else
            {
                c.kind = CommandKind::BadRequest;
                c.error = action.isEmpty() ? QStringLiteral("missing player action")
                                           : QStringLiteral("unknown player action");
                return c;
            }
            return c;
        }

        // ---- #143: the hand-off surface ----
        // /open carries an item REFERENCE plus a position; the body is left untouched for PlayOn::parseHandoff
        // so the routing table here never learns the payload vocabulary. The AUTH decision is not made here
        // either: routing is about shape, and RemoteServer holds the issued tokens.
        if (req.path == QStringLiteral("/open"))
        {
            if (req.method != Method::Post)
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/open is POST only");
                return c;
            }
            if (req.body.trimmed().isEmpty())
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/open needs an item reference");
                return c;
            }
            c.kind = CommandKind::Open;
            return c;
        }

        // /pair with no code ASKS for one (the target puts it on screen); /pair with a code REDEEMS it. The
        // split is by the presence of the parameter rather than by two paths, so a source that has the code
        // and a source that does not use one endpoint and cannot get the order wrong.
        if (req.path == QStringLiteral("/pair"))
        {
            if (req.method != Method::Post)
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/pair is POST only");
                return c;
            }
            const QString code = param(req, body, "code");
            if (code.isNull())
            {
                c.kind = CommandKind::PairBegin;
                return c;
            }
            if (code.isEmpty())
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("empty pairing code");
                return c;
            }
            c.kind = CommandKind::PairRedeem;
            c.pairCode = code;
            return c;
        }

        if (req.path == QStringLiteral("/input"))
        {
            if (req.method != Method::Post)
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/input is POST only");
                return c;
            }
            c.kind = CommandKind::Input;
            const QString dir = param(req, body, "dir").toLower();
            if (dir == QStringLiteral("up"))          c.input = InputDir::Up;
            else if (dir == QStringLiteral("down"))   c.input = InputDir::Down;
            else if (dir == QStringLiteral("left"))   c.input = InputDir::Left;
            else if (dir == QStringLiteral("right"))  c.input = InputDir::Right;
            else if (dir == QStringLiteral("select") || dir == QStringLiteral("ok") || dir == QStringLiteral("enter"))
                                                      c.input = InputDir::Select;
            else if (dir == QStringLiteral("back"))   c.input = InputDir::Back;
            else
            {
                c.kind = CommandKind::BadRequest;
                c.error = dir.isEmpty() ? QStringLiteral("missing input direction")
                                        : QStringLiteral("unknown input direction");
                return c;
            }
            return c;
        }

        c.kind = CommandKind::NotFound;
        c.error = QStringLiteral("no such route");
        return c;
    }

    QByteArray stateJson(const PlayerStateView& s)
    {
        QJsonObject o;
        o.insert(QStringLiteral("hasMedia"), s.hasMedia);
        o.insert(QStringLiteral("playing"),  s.playing);
        o.insert(QStringLiteral("title"),    s.title);
        o.insert(QStringLiteral("position"), s.positionSec);
        o.insert(QStringLiteral("duration"), s.durationSec);
        o.insert(QStringLiteral("volume"),   s.volume);
        o.insert(QStringLiteral("screen"),   s.screen);
        // #143. ADDITIVE: every key above is exactly what #76 shipped, so the phone remote written against
        // that surface still reads this body unchanged. What follows is what a PEER needs to take over --
        // the reference, the selected tracks, and whether a volume exists to move.
        QJsonObject item;
        item.insert(QStringLiteral("kind"),   s.refKind);
        item.insert(QStringLiteral("id"),     s.refId);
        item.insert(QStringLiteral("type"),   s.refType);
        item.insert(QStringLiteral("title"),  s.refTitle);
        item.insert(QStringLiteral("source"), s.refSource);
        o.insert(QStringLiteral("item"), item);
        QJsonObject tracks;
        tracks.insert(QStringLiteral("audio"),    s.audioTrack);
        tracks.insert(QStringLiteral("subtitle"), s.subtitleTrack);
        o.insert(QStringLiteral("tracks"), tracks);
        o.insert(QStringLiteral("volumeControllable"), s.volumeControllable);
        return QJsonDocument(o).toJson(QJsonDocument::Compact);
    }

    const char* reasonPhrase(int status)
    {
        switch (status)
        {
            case 200: return "OK";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";      // #143: an /open from a device that has not paired
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 409: return "Conflict";          // #143: a reference this device cannot resolve
            case 413: return "Payload Too Large";
            default:  return "OK";
        }
    }

    QByteArray httpResponse(int status, const QByteArray& body, const char* contentType)
    {
        QByteArray r = "HTTP/1.1 ";
        r += QByteArray::number(status);
        r += ' ';
        r += reasonPhrase(status);
        r += "\r\n";
        r += "Content-Type: ";
        r += contentType;
        r += "\r\n";
        r += "Content-Length: ";
        r += QByteArray::number(body.size());
        r += "\r\n";
        r += "Connection: close\r\n";
        r += "\r\n";
        r += body;
        return r;
    }
}
