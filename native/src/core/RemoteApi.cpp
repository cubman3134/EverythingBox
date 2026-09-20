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
            if (raw == QStringLiteral("PUT"))  return Method::Put;
            return Method::Other;
        }

        // ---- #423: the origin gate's primitives -------------------------------------------------------
        bool isDigit(char c)    { return c >= '0' && c <= '9'; }
        bool isHexDigit(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
        bool isAlnum(char c)    { return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

        // A dotted quad, strictly: four decimal groups of 0..255 with no leading zero on a multi-digit group
        // (an "010" that one resolver reads as decimal 10 and another as octal 8 is not an accepted spelling
        // of anything here).
        bool ipv4Literal(const QByteArray& s)
        {
            const QList<QByteArray> parts = s.split('.');
            if (parts.size() != 4) return false;
            for (const QByteArray& p : parts)
            {
                if (p.isEmpty() || p.size() > 3) return false;
                for (char c : p) if (!isDigit(c)) return false;
                if (p.size() > 1 && p.at(0) == '0') return false;
                if (p.toInt() > 255) return false;
            }
            return true;
        }

        // The 16-bit groups in one half of an IPv6 literal, or -1 if that half is malformed. A trailing
        // dotted quad counts as two groups and is only allowed where the literal ends.
        int ipv6Groups(const QByteArray& part, bool allowTrailingV4)
        {
            if (part.isEmpty()) return 0;
            const QList<QByteArray> groups = part.split(':');
            int n = 0;
            for (int i = 0; i < groups.size(); ++i)
            {
                const QByteArray g = groups.at(i);
                if (g.isEmpty()) return -1;
                if (g.contains('.'))
                {
                    if (!allowTrailingV4 || i != groups.size() - 1) return -1;
                    if (!ipv4Literal(g)) return -1;
                    n += 2;
                    continue;
                }
                if (g.size() > 4) return -1;
                for (char c : g) if (!isHexDigit(c)) return -1;
                n += 1;
            }
            return n;
        }

        // What goes INSIDE the brackets of a Host: an IPv6 literal, with at most one "::" and an optional
        // zone id (an interface name, "%eth0" or the "%25eth0" a URL spells it as -- it names a link, not a
        // host, so it is checked for shape and then ignored).
        bool ipv6Literal(QByteArray s)
        {
            const int pct = s.indexOf('%');
            if (pct >= 0)
            {
                QByteArray zone = s.mid(pct + 1);
                if (zone.startsWith("25")) zone = zone.mid(2);
                if (zone.isEmpty()) return false;
                for (char c : zone)
                    if (!isAlnum(c) && c != '-' && c != '_' && c != '.') return false;
                s = s.left(pct);
            }
            if (s.isEmpty()) return false;
            const int dc = s.indexOf("::");
            if (dc >= 0 && s.indexOf("::", dc + 1) >= 0) return false;   // ":::" and a second "::" alike
            const QByteArray head = (dc < 0) ? s : s.left(dc);
            const QByteArray tail = (dc < 0) ? QByteArray() : s.mid(dc + 2);
            const int h = ipv6Groups(head, dc < 0 || tail.isEmpty());
            const int t = tail.isEmpty() ? 0 : ipv6Groups(tail, true);
            if (h < 0 || t < 0) return false;
            if (dc < 0) return h == 8;
            return h + t <= 7;      // "::" stands for at least one group, so the halves cannot fill all eight
        }

        // Split a Host-header-shaped authority into its name and port. Refuses everything a host is not
        // allowed to contain -- whitespace, a control byte, non-ASCII, a userinfo '@', a path, a query, a
        // fragment, a backslash -- and any bracket mismatch. `bracketed` marks the [IPv6] form.
        bool splitHostPort(const QByteArray& value, QByteArray& name, QByteArray& port, bool& bracketed)
        {
            name.clear();
            port.clear();
            bracketed = false;
            if (value.isEmpty() || value.size() > 255) return false;
            for (char c : value)
            {
                const unsigned char u = static_cast<unsigned char>(c);
                if (u <= 0x20 || u >= 0x7F) return false;
                if (c == '/' || c == '\\' || c == '@' || c == '?' || c == '#'
                    || c == ',' || c == '"' || c == '<' || c == '>') return false;
            }
            QByteArray rest;
            if (value.startsWith('['))
            {
                const int close = value.indexOf(']');
                if (close < 2) return false;
                name = value.mid(1, close - 1);
                rest = value.mid(close + 1);
                bracketed = true;
            }
            else
            {
                if (value.contains('[') || value.contains(']')) return false;
                const int colon = value.indexOf(':');
                if (colon < 0) { name = value; return !name.isEmpty(); }
                name = value.left(colon);
                rest = value.mid(colon);     // a bare (unbracketed) IPv6 keeps its colons here and is refused
            }
            if (name.isEmpty()) return false;
            if (rest.isEmpty()) return true;
            if (rest.at(0) != ':') return false;
            port = rest.mid(1);
            if (port.isEmpty() || port.size() > 5) return false;
            for (char c : port) if (!isDigit(c)) return false;
            const int p = port.toInt();
            return p >= 1 && p <= 65535;
        }

        // Case-folded, with one trailing dot (the absolute form of the same name) removed.
        QByteArray canonicalName(QByteArray name)
        {
            name = name.toLower();
            if (name.endsWith('.')) name.chop(1);
            return name;
        }

        int effectivePort(const QByteArray& port, int schemeDefault)
        {
            return port.isEmpty() ? schemeDefault : port.toInt();
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
                // #291: the same header as a 64-bit figure, for the streaming decision. The int above is left
                // exactly as it was, so the buffered routes parse bodies as they always have.
                bool ok64 = false;
                const qint64 n64 = line.mid(colon + 1).trimmed().toLongLong(&ok64);
                if (ok64 && n64 >= 0) req.declaredLength = n64;
            }
            else if (name == "content-type")
            {
                QByteArray type = line.mid(colon + 1);
                const int semi = type.indexOf(';');
                if (semi >= 0) type.truncate(semi);
                req.contentType = type.trimmed().toLower();
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
            // #423. Kept, not judged: requestAllowed does the judging, and it is the only caller. Counted,
            // because two Host headers is a malformed request and the gate refuses it rather than picking one.
            else if (name == "host")
            {
                ++req.hostSeen;
                req.host = line.mid(colon + 1).trimmed();
            }
            else if (name == "origin")
            {
                ++req.originSeen;
                req.origin = line.mid(colon + 1).trimmed();
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

        // ---- #127: the library-transfer surface ----
        // GET /inventory is a READ of what this device's art cache holds; POST /bundle carries one item's
        // art. Both are credentialled (PlayOn::routeNeedsToken), and as with /open the AUTH decision is not
        // made here: routing is about shape.
        if (req.path == QStringLiteral("/inventory"))
        {
            if (req.method != Method::Get)
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/inventory is GET only");
                return c;
            }
            c.kind = CommandKind::Inventory;
            return c;
        }

        // #292: what this device's ROM folders hold, for a gamelist diff. A read, credentialled like /inventory.
        if (req.path == QStringLiteral("/gamelists"))
        {
            if (req.method != Method::Get)
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/gamelists is GET only");
                return c;
            }
            c.kind = CommandKind::Gamelists;
            return c;
        }

        // #401: the source's "that system is done" -- the target commits the system's pending gamelist entries.
        // A write, credentialled like /bundle; the body names the system and LibraryBundle decodes it.
        if (req.path == QStringLiteral("/gamelists/flush"))
        {
            if (req.method != Method::Post)
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/gamelists/flush is POST only");
                return c;
            }
            if (req.body.trimmed().isEmpty())
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/gamelists/flush needs a system");
                return c;
            }
            c.kind = CommandKind::GamelistFlush;
            return c;
        }

        if (req.path == QStringLiteral("/bundle"))
        {
            if (req.method != Method::Post)
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/bundle is POST only");
                return c;
            }
            if (req.body.trimmed().isEmpty())
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/bundle needs an item");
                return c;
            }
            c.kind = CommandKind::Bundle;
            return c;
        }

        // ---- #115: the LAN file drop ----
        // GET /drop is the page and only the page: the query is ignored, and nothing about the request picks
        // what is served. Every other /drop route is credentialled (PlayOn::routeNeedsToken covers the whole
        // /drop/ prefix); as everywhere in this file, routing is about shape and FileDrop decides the rest.
        if (req.path == QStringLiteral("/drop"))
        {
            if (req.method != Method::Get)
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/drop is GET only");
                return c;
            }
            c.kind = CommandKind::DropPage;
            return c;
        }
        if (req.path == QStringLiteral("/drop/destinations"))
        {
            if (req.method != Method::Get)
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("/drop/destinations is GET only");
                return c;
            }
            c.kind = CommandKind::DropDestinations;
            return c;
        }
        if (req.path == QStringLiteral("/drop/start") || req.path == QStringLiteral("/drop/finish"))
        {
            const bool start = req.path == QStringLiteral("/drop/start");
            if (req.method != Method::Post)
            {
                c.kind = CommandKind::BadRequest;
                c.error = start ? QStringLiteral("/drop/start is POST only") : QStringLiteral("/drop/finish is POST only");
                return c;
            }
            if (req.body.trimmed().isEmpty())
            {
                c.kind = CommandKind::BadRequest;
                c.error = start ? QStringLiteral("/drop/start needs a destination, a name and a size")
                                : QStringLiteral("/drop/finish needs an upload id");
                return c;
            }
            c.kind = start ? CommandKind::DropStart : CommandKind::DropFinish;
            return c;
        }
        if (req.path == QStringLiteral("/drop/status") || req.path == QStringLiteral("/drop/chunk"))
        {
            const bool chunk = req.path == QStringLiteral("/drop/chunk");
            if (req.method != (chunk ? Method::Put : Method::Get))
            {
                c.kind = CommandKind::BadRequest;
                c.error = chunk ? QStringLiteral("/drop/chunk is PUT only") : QStringLiteral("/drop/status is GET only");
                return c;
            }
            c.dropId = req.query.value(QStringLiteral("id"));
            if (c.dropId.isEmpty())
            {
                c.kind = CommandKind::BadRequest;
                c.error = QStringLiteral("an upload id is needed");
                return c;
            }
            if (chunk)
            {
                bool ok = false;
                const qint64 off = req.query.value(QStringLiteral("offset")).toLongLong(&ok);
                if (!ok || off < 0)
                {
                    c.kind = CommandKind::BadRequest;
                    c.error = QStringLiteral("a piece needs its offset");
                    return c;
                }
                c.dropOffset = off;
            }
            c.kind = chunk ? CommandKind::DropChunk : CommandKind::DropStatus;
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
            case 411: return "Length Required";   // #291: a streamed bundle must say where it ends
            case 413: return "Payload Too Large";
            case 500: return "Internal Server Error";
            case 503: return "Service Unavailable";
            case 507: return "Insufficient Storage";   // #115: a file drop larger than the free space
            default:  return "OK";
        }
    }

    QByteArray httpResponse(int status, const QByteArray& body, const char* contentType)
    {
        return httpResponse(status, body, contentType, {});
    }

    QByteArray httpResponse(int status, const QByteArray& body, const char* contentType,
                            const QList<QByteArray>& extraHeaders)
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
        for (const QByteArray& h : extraHeaders)
        {
            // A header value that carried a line break would split the response; such a line is dropped.
            if (h.contains('\r') || h.contains('\n')) continue;
            r += h;
            r += "\r\n";
        }
        r += "\r\n";
        r += body;
        return r;
    }

    bool hostAllowed(const QByteArray& hostHeaderValue, const QString& selfLocalName)
    {
        QByteArray name, port;
        bool bracketed = false;
        if (!splitHostPort(hostHeaderValue.trimmed(), name, port, bracketed)) return false;
        if (bracketed) return ipv6Literal(name);
        const QByteArray n = canonicalName(name);
        if (n.isEmpty()) return false;
        if (n == "localhost") return true;
        if (ipv4Literal(n)) return true;
        // The ONE DNS name this device answers to: its own mDNS name, compared EXACTLY. A suffix match would
        // accept "<id>.local.evil.com"; a prefix match would accept "evil-<id>.local"; both are other names.
        const QByteArray self = canonicalName(selfLocalName.trimmed().toUtf8());
        if (self.size() <= 6 || !self.endsWith(".local")) return false;
        return n == self;
    }

    namespace
    {
        // An Origin, when the browser sends one: a serialized origin ("http://192.168.1.5:8080"), which must
        // pass the same host rule AND name the same origin the request's own Host does. "null" (a sandboxed
        // frame, a file:// page), an opaque value and every cross-origin one fall out here.
        bool originMatchesHost(const QByteArray& origin, const QByteArray& hostHeader, const QString& selfLocalName)
        {
            const QByteArray o = origin.trimmed();
            const int sep = o.indexOf("://");
            if (sep <= 0) return false;
            // Only http: this listener is plaintext, so no https page was ever loaded FROM it and no https
            // origin can be the same origin as its Host. "null", "file://" and an extension's scheme fall
            // out here too.
            if (o.left(sep).toLower() != "http") return false;
            const int schemeDefault = 80;
            const QByteArray authority = o.mid(sep + 3);
            if (!hostAllowed(authority, selfLocalName)) return false;
            QByteArray oName, oPort, hName, hPort;
            bool oBracketed = false, hBracketed = false;
            if (!splitHostPort(authority, oName, oPort, oBracketed)) return false;
            if (!splitHostPort(hostHeader.trimmed(), hName, hPort, hBracketed)) return false;
            if (oBracketed != hBracketed) return false;
            if (canonicalName(oName) != canonicalName(hName)) return false;
            // The Host header's own default is 80: a browser that omitted the port from one omitted it from
            // the other, because both come from the same address bar.
            return effectivePort(oPort, schemeDefault) == effectivePort(hPort, 80);
        }
    }

    bool requestAllowed(const Request& req, const QString& selfLocalName)
    {
        // An unparseable request reaches no route at all (it is answered 400 either way) and carries no Host
        // worth weighing. The gate speaks for the parseable ones, which is every request an attacker sends.
        if (!req.valid) return true;
        // No preflight is ever answered: nothing here is reachable cross-origin, so nothing needs one.
        if (req.methodRaw.compare(QStringLiteral("OPTIONS"), Qt::CaseInsensitive) == 0) return false;
        if (req.hostSeen != 1) return false;                        // missing, or sent more than once
        if (!hostAllowed(req.host, selfLocalName)) return false;
        if (req.originSeen == 0) return true;                       // a same-origin fetch may send none
        if (req.originSeen != 1) return false;
        return originMatchesHost(req.origin, req.host, selfLocalName);
    }

    QByteArray forbiddenResponse()
    {
        return httpResponse(403, "forbidden\n", "text/plain");
    }

    int requestCapBytes(const QByteArray& rawPrefix)
    {
        // Matched on the request LINE, before any header is trusted, and only for the exact spelling the
        // bundle client sends. A request that merely mentions /bundle later (in a header, in a body) does not
        // get the larger cap: the target of a POST is the first thing on the wire or it is not this route.
        if (rawPrefix.startsWith("POST /bundle ") || rawPrefix.startsWith("POST /bundle?"))
            return kBundleRequestCap;
        return kDefaultRequestCap;
    }

    BodyPlan bodyPlanFor(const Request& headers)
    {
        // Only POST /bundle, and only when the body declares itself as the raw-body format. The JSON (v1)
        // bundle, every other route, and a request that names the type anywhere but Content-Type are buffered
        // exactly as they always were.
        // #115: a file-drop piece is streamed into its upload's part file. Its ceiling is FileDrop's piece size.
        if (isDropChunk(headers))
        {
            if (headers.declaredLength < 0) return BodyPlan::LengthRequired;
            if (headers.declaredLength > kDropChunkStreamCap) return BodyPlan::TooLarge;
            return BodyPlan::Stream;
        }
        if (!headers.valid || headers.method != Method::Post) return BodyPlan::Buffer;
        if (headers.path != QStringLiteral("/bundle")) return BodyPlan::Buffer;
        if (headers.contentType != QByteArray(kBundleStreamContentType)) return BodyPlan::Buffer;
        if (headers.declaredLength < 0) return BodyPlan::LengthRequired;
        if (headers.declaredLength > kBundleStreamCap) return BodyPlan::TooLarge;
        return BodyPlan::Stream;
    }

    bool isDropChunk(const Request& headers)
    {
        return headers.valid && headers.method == Method::Put && headers.path == QStringLiteral("/drop/chunk");
    }
}
