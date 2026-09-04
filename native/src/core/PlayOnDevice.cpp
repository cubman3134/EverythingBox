#include "PlayOnDevice.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace PlayOn
{
namespace
{
    // ---- DNS name coding ----------------------------------------------------------------------------------
    // Uncompressed on the way out (a 4-record answer is well under the MTU and compression buys nothing but a
    // second way to be wrong); pointer-following on the way in, because other responders do compress.

    QByteArray encodeName(const QByteArray& fqdn)
    {
        QByteArray o;
        for (const QByteArray& label : fqdn.split('.'))
        {
            if (label.isEmpty()) continue;                 // a trailing dot, or a doubled one
            const QByteArray l = label.left(63);           // a DNS label caps at 63 bytes
            o.append(char(l.size()));
            o += l;
        }
        o.append(char(0));
        return o;
    }

    // Decode the name at `pos`. `next` receives the offset just past the name AS ENCODED AT pos (so a caller
    // walking records advances correctly even when the name was a pointer). Returns a lowercase dotted name.
    // Bounded by a hop budget so a self-referential pointer cannot spin.
    QByteArray decodeName(const QByteArray& pkt, int pos, int* next)
    {
        QByteArray out;
        const int n = pkt.size();
        int hops = 0;
        bool jumped = false;
        if (next) *next = pos;
        while (pos >= 0 && pos < n)
        {
            const quint8 len = quint8(pkt[pos]);
            if (len == 0)
            {
                if (!jumped && next) *next = pos + 1;
                break;
            }
            if ((len & 0xC0) == 0xC0)
            {
                if (pos + 1 >= n) break;
                if (!jumped && next) *next = pos + 2;
                jumped = true;
                if (++hops > 16) break;                     // a pointer loop; stop rather than spin
                pos = (int(len & 0x3F) << 8) | quint8(pkt[pos + 1]);
                continue;
            }
            if (pos + 1 + int(len) > n) break;
            if (!out.isEmpty()) out.append('.');
            out += pkt.mid(pos + 1, int(len));
            pos += 1 + int(len);
            if (!jumped && next) *next = pos;
        }
        return out.toLower();
    }

    void put16(QByteArray& b, quint16 v) { b.append(char(v >> 8)); b.append(char(v & 0xFF)); }
    void put32(QByteArray& b, quint32 v)
    {
        b.append(char((v >> 24) & 0xFF)); b.append(char((v >> 16) & 0xFF));
        b.append(char((v >> 8) & 0xFF));  b.append(char(v & 0xFF));
    }
    int u16(const QByteArray& b, int p) { return int((quint8(b[p]) << 8) | quint8(b[p + 1])); }

    QByteArray instanceFqdn(const QString& instanceId)
    {
        return instanceId.toUtf8() + '.' + QByteArray(kServiceDomain);
    }
    QByteArray hostFqdn(const QString& instanceId)
    {
        return instanceId.toUtf8() + ".local";
    }

    QString dotted(quint32 ip)
    {
        return QStringLiteral("%1.%2.%3.%4")
            .arg((ip >> 24) & 0xFF).arg((ip >> 16) & 0xFF).arg((ip >> 8) & 0xFF).arg(ip & 0xFF);
    }

    // The /state "item" object's keys, in one place. RemoteApi::stateJson writes them; continueHere reads
    // them; a drift between the two would present as "Continue on this device" silently refusing everything.
    constexpr const char* kItemObj  = "item";
    constexpr const char* kTracks   = "tracks";
}

// ------------------------------------------------------------------ 1. the advertisement ------------------

bool advertValid(const Advert& a)
{
    return !a.instanceId.isEmpty() && a.port != 0;
}

QList<QByteArray> txtRecords(const Advert& a)
{
    // Pinned order: name, ver, id.
    QList<QByteArray> t;
    t << ("name=" + a.name.toUtf8());
    t << ("ver=" + a.version.toUtf8());
    t << ("id=" + a.instanceId.toUtf8());
    return t;
}

// ------------------------------------------------------------------ 2. the wire ---------------------------

QByteArray mdnsQuery()
{
    QByteArray q;
    put16(q, 0);        // id
    put16(q, 0);        // flags: a standard query
    put16(q, 1);        // qdcount
    put16(q, 0);        // ancount
    put16(q, 0);        // nscount
    put16(q, 0);        // arcount
    q += encodeName(QByteArray(kServiceDomain));
    put16(q, 12);       // QTYPE = PTR
    put16(q, 1);        // QCLASS = IN
    return q;
}

bool isServiceQuery(const QByteArray& pkt)
{
    if (pkt.size() < 12) return false;
    if (quint8(pkt[2]) & 0x80) return false;               // QR set => this is a response, not a question
    const int qd = u16(pkt, 4);
    if (qd <= 0) return false;
    int p = 12;
    const QByteArray want = QByteArray(kServiceDomain).toLower();
    for (int i = 0; i < qd && p < pkt.size(); ++i)
    {
        int next = p;
        const QByteArray name = decodeName(pkt, p, &next);
        p = next + 4;                                       // qtype + qclass
        if (p > pkt.size()) return false;
        // PTR (12) or ANY (255) for our service type — both are questions we can answer.
        if (name == want) return true;
    }
    return false;
}

QByteArray mdnsResponse(const Advert& a, quint32 ipv4)
{
    QByteArray r;
    if (!advertValid(a)) return r;

    const QList<QByteArray> txt = txtRecords(a);
    QByteArray txtRdata;
    for (const QByteArray& s : txt) { txtRdata.append(char(s.left(255).size())); txtRdata += s.left(255); }

    const QByteArray svc  = QByteArray(kServiceDomain);
    const QByteArray inst = instanceFqdn(a.instanceId);
    const QByteArray host = hostFqdn(a.instanceId);

    const quint16 an = ipv4 ? 4 : 3;
    put16(r, 0);            // id
    put16(r, 0x8400);       // QR + AA
    put16(r, 0);            // qdcount
    put16(r, an);
    put16(r, 0);
    put16(r, 0);

    // PTR: _everythingbox._tcp.local -> <id>._everythingbox._tcp.local
    {
        r += encodeName(svc);
        put16(r, 12); put16(r, 1); put32(r, 120);
        const QByteArray rd = encodeName(inst);
        put16(r, quint16(rd.size())); r += rd;
    }
    // SRV: <id>._everythingbox._tcp.local -> 0 0 <port> <id>.local
    {
        r += encodeName(inst);
        put16(r, 33); put16(r, 0x8001); put32(r, 120);      // cache-flush | IN
        QByteArray rd;
        put16(rd, 0); put16(rd, 0); put16(rd, a.port);
        rd += encodeName(host);
        put16(r, quint16(rd.size())); r += rd;
    }
    // TXT: name= / ver= / id=
    {
        r += encodeName(inst);
        put16(r, 16); put16(r, 0x8001); put32(r, 120);
        put16(r, quint16(txtRdata.size())); r += txtRdata;
    }
    // A: <id>.local -> ipv4 (omitted when the caller has no address to publish)
    if (ipv4)
    {
        r += encodeName(host);
        put16(r, 1); put16(r, 0x8001); put32(r, 120);
        put16(r, 4); put32(r, ipv4);
    }
    return r;
}

bool parseAdvert(const QByteArray& pkt, quint32 senderIpv4, Peer& out)
{
    const int n = pkt.size();
    if (n < 12) return false;
    if (!(quint8(pkt[2]) & 0x80)) return false;             // only a response carries an advertisement

    const int qd = u16(pkt, 4);
    const int total = u16(pkt, 6) + u16(pkt, 8) + u16(pkt, 10);
    int p = 12;
    for (int i = 0; i < qd && p < n; ++i) { int next = p; decodeName(pkt, p, &next); p = next + 4; }

    const QByteArray svcSuffix = QByteArray(kServiceDomain).toLower();
    Peer peer;
    QString aRecordIp;
    QByteArray srvTarget;

    for (int i = 0; i < total && p < n; ++i)
    {
        int next = p;
        const QByteArray name = decodeName(pkt, p, &next);
        p = next;
        if (p + 10 > n) break;
        const int type  = u16(pkt, p);
        const int rdlen = u16(pkt, p + 8);
        p += 10;
        if (p + rdlen > n) break;
        const bool oursByName = name.endsWith(svcSuffix);

        if (type == 16 && oursByName)                       // TXT for our service
        {
            int q = p;
            while (q < p + rdlen)
            {
                const int slen = quint8(pkt[q++]);
                if (slen == 0 || q + slen > p + rdlen) break;
                const QByteArray kv = pkt.mid(q, slen);
                q += slen;
                const int eq = kv.indexOf('=');
                if (eq <= 0) continue;
                const QByteArray k = kv.left(eq);
                const QString v = QString::fromUtf8(kv.mid(eq + 1));
                if      (k == "id")   peer.id = v;
                else if (k == "name") peer.name = v;
                else if (k == "ver")  peer.version = v;
            }
        }
        else if (type == 33 && oursByName && rdlen >= 7)     // SRV for our service
        {
            peer.port = quint16(u16(pkt, p + 4));
            int dummy = 0;
            srvTarget = decodeName(pkt, p + 6, &dummy);
        }
        else if (type == 1 && rdlen == 4 && aRecordIp.isEmpty())
        {
            aRecordIp = QStringLiteral("%1.%2.%3.%4")
                            .arg(quint8(pkt[p])).arg(quint8(pkt[p + 1]))
                            .arg(quint8(pkt[p + 2])).arg(quint8(pkt[p + 3]));
        }
        p += rdlen;
    }

    if (peer.id.isEmpty() || peer.port == 0) return false;
    peer.host = !aRecordIp.isEmpty() ? aRecordIp
                                     : (senderIpv4 ? dotted(senderIpv4) : QString());
    if (peer.host.isEmpty()) return false;                   // nothing to connect to
    if (peer.name.isEmpty()) peer.name = peer.id;
    out = peer;
    return true;
}

// ------------------------------------------------------------------ 3. the picker merge -------------------

QString peerTargetId(const QString& instanceId) { return QStringLiteral("eb:") + instanceId; }

QString peerLabel(const QString& name)
{
    return QStringLiteral("EverythingBox on ") + name;
}

QList<Target> mergeTargets(const QList<Target>& castTargets,
                           const QList<Peer>& peers,
                           const QString& selfInstanceId,
                           const QSet<QString>& pairedIds)
{
    QList<Target> out;
    QSet<QString> seen;

    for (const Peer& p : peers)
    {
        if (p.id.isEmpty()) continue;
        if (!selfInstanceId.isEmpty() && p.id == selfInstanceId) continue;   // never list ourselves
        Target t;
        t.kind   = TargetKind::EverythingBox;
        t.id     = peerTargetId(p.id);
        t.name   = p.name;
        t.label  = peerLabel(p.name);
        t.host   = p.host;
        t.port   = p.port;
        t.paired = pairedIds.contains(p.id);
        if (seen.contains(t.id)) continue;
        seen.insert(t.id);
        out << t;
    }
    for (const Target& c : castTargets)
    {
        if (c.id.isEmpty() || seen.contains(c.id)) continue;
        seen.insert(c.id);
        Target t = c;
        if (t.label.isEmpty()) t.label = t.name;
        out << t;
    }
    return out;
}

// ------------------------------------------------------------------ 4. the hand-off contract --------------

QByteArray handoffJson(const Handoff& h)
{
    QJsonObject ref;
    ref.insert(QStringLiteral("kind"),   h.ref.kind);
    ref.insert(QStringLiteral("id"),     h.ref.id);
    ref.insert(QStringLiteral("type"),   h.ref.type);
    ref.insert(QStringLiteral("title"),  h.ref.title);
    ref.insert(QStringLiteral("source"), h.ref.source);

    QJsonObject tracks;
    tracks.insert(QStringLiteral("audio"),    h.audioTrack);
    tracks.insert(QStringLiteral("subtitle"), h.subtitleTrack);

    QJsonObject o;
    o.insert(QStringLiteral("ref"),      ref);
    o.insert(QStringLiteral("position"), h.positionSec);
    o.insert(QStringLiteral("tracks"),   tracks);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

bool parseHandoff(const QByteArray& json, Handoff& out, QString& error)
{
    error.clear();
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) { error = QStringLiteral("body is not a JSON object"); return false; }
    const QJsonObject o = doc.object();
    const QJsonValue rv = o.value(QStringLiteral("ref"));
    if (!rv.isObject()) { error = QStringLiteral("missing item reference"); return false; }
    const QJsonObject ro = rv.toObject();

    Handoff h;
    h.ref.kind   = ro.value(QStringLiteral("kind")).toString();
    h.ref.id     = ro.value(QStringLiteral("id")).toString();
    h.ref.type   = ro.value(QStringLiteral("type")).toString();
    h.ref.title  = ro.value(QStringLiteral("title")).toString();
    h.ref.source = ro.value(QStringLiteral("source")).toString();
    h.positionSec = o.value(QStringLiteral("position")).toDouble(0.0);
    if (h.positionSec < 0.0) h.positionSec = 0.0;           // a negative resume point is a broken source
    const QJsonObject tr = o.value(QStringLiteral("tracks")).toObject();
    h.audioTrack    = tr.value(QStringLiteral("audio")).toString();
    h.subtitleTrack = tr.value(QStringLiteral("subtitle")).toString();
    out = h;
    return true;
}

OpenResult decideOpen(const Handoff& h, const OpenEnv& env)
{
    OpenResult r;

    const QString kind = h.ref.kind;
    if (h.ref.id.isEmpty() || kind.isEmpty())
    {
        r.outcome = OpenOutcome::BadRequest; r.httpStatus = 400;
        r.reason = QStringLiteral("the hand-off carried no item reference");
        return r;
    }
    if (kind != QLatin1String("catalog") && kind != QLatin1String("addon")
        && kind != QLatin1String("server") && kind != QLatin1String("local"))
    {
        r.outcome = OpenOutcome::BadRequest; r.httpStatus = 400;
        r.reason = QStringLiteral("unknown reference kind");
        return r;
    }

    // The profile gate runs BEFORE the lookup: a target behind a passcode must not reveal, by answering 409
    // rather than 403, whether it could have played the item.
    if (env.profileBlocks)
    {
        r.outcome = OpenOutcome::Gated; r.httpStatus = 403;
        r.reason = env.blockReason.isEmpty()
                       ? QStringLiteral("the profile on that device does not allow this")
                       : env.blockReason;
        return r;
    }

    bool resolvable = false;
    QString why;
    if (kind == QLatin1String("local"))
    {
        resolvable = env.localIdKnown;
        why = QStringLiteral("that file is not in this device's library");
    }
    else if (kind == QLatin1String("server"))
    {
        resolvable = env.serverConfigured;
        why = QStringLiteral("this device is not connected to that server");
    }
    else
    {
        resolvable = env.addonsAvailable;
        why = QStringLiteral("no source on this device can resolve that item");
    }
    if (!resolvable)
    {
        r.outcome = OpenOutcome::Unresolvable; r.httpStatus = 409; r.reason = why;
        return r;
    }

    r.outcome = OpenOutcome::Accepted; r.httpStatus = 200; r.reason.clear();
    return r;
}

QByteArray openResultJson(const OpenResult& r)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), r.outcome == OpenOutcome::Accepted);
    o.insert(QStringLiteral("status"), r.httpStatus);
    o.insert(QStringLiteral("reason"), r.reason);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QString describeRefusal(const OpenResult& r, const QString& deviceName)
{
    if (r.outcome == OpenOutcome::Accepted) return QString();
    const QString who = deviceName.isEmpty() ? QStringLiteral("that device") : deviceName;
    if (r.outcome == OpenOutcome::Unresolvable)
        return QStringLiteral("Not available on ") + who + QStringLiteral(" — ") + r.reason;
    if (r.outcome == OpenOutcome::Gated)
        return QStringLiteral("Blocked on ") + who + QStringLiteral(" — ") + r.reason;
    return who + QStringLiteral(" refused the hand-off — ") + r.reason;
}

// ------------------------------------------------------------------ 5. pairing ----------------------------

QString pairingCode(quint32 entropy)
{
    return QStringLiteral("%1").arg(entropy % 1000000u, 6, 10, QLatin1Char('0'));
}

QString normalizeCode(const QString& code)
{
    QString o;
    for (const QChar c : code)
        if (!c.isSpace() && c != QLatin1Char('-')) o.append(c);
    return o;
}

bool codeMatches(const QString& expected, const QString& entered)
{
    const QString e = normalizeCode(expected);
    if (e.isEmpty()) return false;                          // no offer is armed: nothing matches
    return e == normalizeCode(entered);
}

QString mintToken(const QByteArray& entropy)
{
    // Hashed rather than used raw so the stored credential never carries the caller's entropy verbatim, and so
    // the length is fixed whatever the caller passes.
    return QString::fromLatin1(QCryptographicHash::hash(entropy, QCryptographicHash::Sha256).toHex());
}

QString Pairing::begin(quint32 entropy)
{
    code_ = pairingCode(entropy);
    attempts_ = 0;
    pending_ = true;
    return code_;
}

void Pairing::cancel()
{
    pending_ = false;
    code_.clear();
    attempts_ = 0;
}

QString Pairing::redeem(const QString& entered, const QByteArray& tokenEntropy)
{
    if (!pending_) return QString();
    if (!codeMatches(code_, entered))
    {
        if (++attempts_ >= kMaxAttempts) cancel();          // burn the offer; the code must be re-shown
        return QString();
    }
    const QString token = mintToken(tokenEntropy);
    cancel();                                               // one-shot: a code is never reusable
    return token;
}

// ------------------------------------------------------------------ 6. auth -------------------------------

bool routeNeedsToken(const QString& path)
{
    return path == QLatin1String("/open");
}

bool authorized(const QString& presented, const QSet<QString>& issuedTokens)
{
    if (presented.isEmpty()) return false;
    return issuedTokens.contains(presented);
}

QByteArray unauthorizedJson()
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), false);
    o.insert(QStringLiteral("status"), 401);
    o.insert(QStringLiteral("reason"), QStringLiteral("pair this device first"));
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// ------------------------------------------------------------------ 7. remote mode ------------------------

RemoteView remoteView(const QByteArray& stateJson, bool httpOk)
{
    RemoteView v;
    if (!httpOk) return v;                                   // unreachable: every other field stays at rest
    const QJsonDocument doc = QJsonDocument::fromJson(stateJson);
    if (!doc.isObject()) return v;
    const QJsonObject o = doc.object();
    v.reachable   = true;
    v.hasMedia    = o.value(QStringLiteral("hasMedia")).toBool();
    v.playing     = o.value(QStringLiteral("playing")).toBool();
    v.title       = o.value(QStringLiteral("title")).toString();
    v.positionSec = o.value(QStringLiteral("position")).toDouble();
    v.durationSec = o.value(QStringLiteral("duration")).toDouble();
    v.volume      = o.value(QStringLiteral("volume")).toInt();
    // Volume is offered only when the target says it owns one. A slider that moves nothing reads as a broken
    // remote, so absent means NOT controllable rather than "try it and see".
    v.volumeControllable = o.value(QStringLiteral("volumeControllable")).toBool(false);
    return v;
}

QByteArray playerCommandBody(const QString& action)
{
    QJsonObject o;
    o.insert(QStringLiteral("action"), action);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray seekCommandBody(double positionSec)
{
    QJsonObject o;
    o.insert(QStringLiteral("action"), QStringLiteral("seek"));
    o.insert(QStringLiteral("pos"), positionSec < 0.0 ? 0.0 : positionSec);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray volumeCommandBody(int level)
{
    QJsonObject o;
    o.insert(QStringLiteral("action"), QStringLiteral("volume"));
    o.insert(QStringLiteral("level"), level < 0 ? 0 : (level > 100 ? 100 : level));
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// ------------------------------------------------------------------ 8. continue here ----------------------

Pull continueHere(const QByteArray& stateJson, bool httpOk)
{
    Pull p;
    if (!httpOk) { p.reason = QStringLiteral("that device did not answer"); return p; }
    const QJsonDocument doc = QJsonDocument::fromJson(stateJson);
    if (!doc.isObject()) { p.reason = QStringLiteral("that device sent something unreadable"); return p; }
    const QJsonObject o = doc.object();
    if (!o.value(QStringLiteral("hasMedia")).toBool())
    {
        p.reason = QStringLiteral("nothing is playing there");
        return p;
    }
    const QJsonObject ro = o.value(QLatin1String(kItemObj)).toObject();
    ItemRef ref;
    ref.kind   = ro.value(QStringLiteral("kind")).toString();
    ref.id     = ro.value(QStringLiteral("id")).toString();
    ref.type   = ro.value(QStringLiteral("type")).toString();
    ref.title  = ro.value(QStringLiteral("title")).toString();
    ref.source = ro.value(QStringLiteral("source")).toString();
    if (ref.kind.isEmpty() || ref.id.isEmpty())
    {
        // The peer is playing, but cannot say WHAT — an external file dropped on it, say. There is nothing
        // here this device could open, and guessing from the title would open the wrong thing.
        p.reason = QStringLiteral("that device cannot name what it is playing");
        return p;
    }
    const QJsonObject tr = o.value(QLatin1String(kTracks)).toObject();
    p.valid         = true;
    p.ref           = ref;
    p.positionSec   = o.value(QStringLiteral("position")).toDouble(0.0);
    if (p.positionSec < 0.0) p.positionSec = 0.0;
    p.audioTrack    = tr.value(QStringLiteral("audio")).toString();
    p.subtitleTrack = tr.value(QStringLiteral("subtitle")).toString();
    return p;
}

Handoff handoffFromPull(const Pull& p)
{
    Handoff h;
    h.ref           = p.ref;
    h.positionSec   = p.positionSec;
    h.audioTrack    = p.audioTrack;
    h.subtitleTrack = p.subtitleTrack;
    return h;
}

} // namespace PlayOn
