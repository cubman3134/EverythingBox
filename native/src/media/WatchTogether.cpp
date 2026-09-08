#include "WatchTogether.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace
{
    // The wire spellings, in one table so the encoder and the parser cannot drift.
    struct TypeName { WatchTogether::MsgType t; const char* id; };
    const TypeName kTypeNames[] = {
        { WatchTogether::MsgType::Hello,        "hello"   },
        { WatchTogether::MsgType::Roster,       "roster"  },
        { WatchTogether::MsgType::Item,         "item"    },
        { WatchTogether::MsgType::Transport,    "xport"   },
        { WatchTogether::MsgType::Beacon,       "beacon"  },
        { WatchTogether::MsgType::Buffering,    "buf"     },
        { WatchTogether::MsgType::RequestPause, "reqpause"},
        { WatchTogether::MsgType::RequestSeek,  "reqseek" },
        { WatchTogether::MsgType::Unresolved,   "unres"   },
        { WatchTogether::MsgType::Bye,          "bye"     },
    };

    // Query keys and header values that carry an account. Lower-cased before the test.
    const char* const kCredentialMarkers[] = {
        "token=", "apikey=", "api_key=", "access_key", "accesstoken", "auth=", "authorization",
        "signature=", "sig=", "x-amz-", "bearer ", "password=", "passwd=", "secret=", "session_id=",
    };

    // A run of this many unbroken hex digits is a token, a key or an infohash, never a title and never a
    // path component anyone types.
    constexpr int kHexRunThreshold = 32;

    bool hasLongHexRun(const QString& s)
    {
        int run = 0;
        for (int i = 0; i < s.size(); ++i)
        {
            const QChar c = s.at(i);
            const bool hex = (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                          || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
                          || (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
            run = hex ? run + 1 : 0;
            if (run >= kHexRunThreshold) return true;
        }
        return false;
    }

    double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
}

namespace WatchTogether
{

// ---- 1. the room code ------------------------------------------------------------------------------------

QString makeRoomCode(quint32 entropy)
{
    const QString alphabet = QString::fromLatin1(kCodeAlphabet);
    QString out;
    quint32 e = entropy;
    for (int i = 0; i < kCodeLength; ++i)
    {
        out += alphabet.at(int(e % quint32(alphabet.size())));
        e /= quint32(alphabet.size());
    }
    return out;
}

QString normalizeCode(const QString& typed)
{
    QString out;
    for (int i = 0; i < typed.size(); ++i)
    {
        const QChar c = typed.at(i);
        if (c.isSpace() || c == QLatin1Char('-') || c == QLatin1Char('_')) continue;
        out += c.toUpper();
    }
    return out;
}

bool codeValid(const QString& code)
{
    if (code.size() != kCodeLength) return false;
    const QString alphabet = QString::fromLatin1(kCodeAlphabet);
    for (int i = 0; i < code.size(); ++i)
        if (!alphabet.contains(code.at(i))) return false;
    return true;
}

// ---- 2. the credential rule ------------------------------------------------------------------------------

bool looksLikeCredential(const QString& s)
{
    if (s.isEmpty()) return false;
    // Any url. A resolved stream always has a scheme, and nothing this feature legitimately shares does:
    // a catalogue id is an id, and a local reference is a filesystem path.
    if (s.contains(QLatin1String("://"))) return true;
    const QString lower = s.toLower();
    for (const char* marker : kCredentialMarkers)
        if (lower.contains(QLatin1String(marker))) return true;
    return hasLongHexRun(s);
}

QString scrubForWire(const QString& s) { return looksLikeCredential(s) ? QString() : s; }

PlayOn::ItemRef shareableRef(const PlayOn::ItemRef& in)
{
    PlayOn::ItemRef out;
    out.kind   = scrubForWire(in.kind);
    out.id     = scrubForWire(in.id);
    out.type   = scrubForWire(in.type);
    out.title  = scrubForWire(in.title);
    out.source = scrubForWire(in.source);
    return out;
}

bool refShareable(const PlayOn::ItemRef& in)
{
    // A reference whose ID is scrubbed away is one no peer could resolve. Sending it would put a row on the
    // other screen that fails the moment it is pressed, so the sender is told NOT to send it at all.
    return !in.id.isEmpty() && !looksLikeCredential(in.id) && !in.kind.isEmpty();
}

// ---- 3. the message set ----------------------------------------------------------------------------------

QString typeId(MsgType t)
{
    for (const TypeName& n : kTypeNames)
        if (n.t == t) return QString::fromLatin1(n.id);
    return QString();
}

MsgType typeFromId(const QString& id)
{
    for (const TypeName& n : kTypeNames)
        if (id == QLatin1String(n.id)) return n.t;
    return MsgType::Unknown;
}

QByteArray encode(const Message& m)
{
    if (m.type == MsgType::Unknown) return QByteArray();
    QJsonObject o;
    o.insert(QStringLiteral("t"), typeId(m.type));
    o.insert(QStringLiteral("v"), m.protocol);
    if (!m.participantId.isEmpty()) o.insert(QStringLiteral("pid"), scrubForWire(m.participantId));
    if (!m.displayName.isEmpty())   o.insert(QStringLiteral("name"), scrubForWire(m.displayName));
    if (m.host) o.insert(QStringLiteral("host"), true);

    if (m.type == MsgType::Item)
    {
        // The identity, scrubbed field by field. This is the line the whole feature turns on: what goes out
        // is a REFERENCE, and a caller that hands in a resolved url gets an empty field, not a leak.
        const PlayOn::ItemRef r = shareableRef(m.ref);
        QJsonObject ro;
        ro.insert(QStringLiteral("kind"), r.kind);
        ro.insert(QStringLiteral("id"), r.id);
        ro.insert(QStringLiteral("type"), r.type);
        ro.insert(QStringLiteral("title"), r.title);
        ro.insert(QStringLiteral("source"), r.source);
        o.insert(QStringLiteral("ref"), ro);
    }
    if (m.type == MsgType::Item || m.type == MsgType::Transport || m.type == MsgType::Beacon
        || m.type == MsgType::Buffering || m.type == MsgType::RequestSeek)
        o.insert(QStringLiteral("pos"), m.positionSec);
    if (m.type == MsgType::Item || m.type == MsgType::Transport || m.type == MsgType::Beacon
        || m.type == MsgType::RequestPause)
        o.insert(QStringLiteral("paused"), m.paused);
    if (m.type == MsgType::Beacon) o.insert(QStringLiteral("clock"), double(m.clockMs));
    if (m.type == MsgType::Buffering) o.insert(QStringLiteral("buffering"), m.buffering);
    if (!m.reason.isEmpty()) o.insert(QStringLiteral("why"), scrubForWire(m.reason));
    if (m.type == MsgType::Roster)
    {
        QJsonArray arr;
        for (const RosterEntry& e : m.roster)
        {
            QJsonObject eo;
            eo.insert(QStringLiteral("id"), scrubForWire(e.id));
            eo.insert(QStringLiteral("name"), scrubForWire(e.name));
            eo.insert(QStringLiteral("host"), e.host);
            eo.insert(QStringLiteral("buffering"), e.buffering);
            eo.insert(QStringLiteral("resolved"), e.resolved);
            arr.append(eo);
        }
        o.insert(QStringLiteral("roster"), arr);
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

bool decode(const QByteArray& line, Message& out, QString& error)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
    {
        error = QStringLiteral("not a JSON object");
        return false;
    }
    const QJsonObject o = doc.object();
    out = Message();
    out.type = typeFromId(o.value(QStringLiteral("t")).toString());
    if (out.type == MsgType::Unknown) { error = QStringLiteral("unknown message type"); return false; }
    out.protocol      = o.value(QStringLiteral("v")).toInt(0);
    out.participantId = o.value(QStringLiteral("pid")).toString();
    out.displayName   = o.value(QStringLiteral("name")).toString();
    out.host          = o.value(QStringLiteral("host")).toBool(false);
    out.positionSec   = o.value(QStringLiteral("pos")).toDouble(0.0);
    out.paused        = o.value(QStringLiteral("paused")).toBool(false);
    out.clockMs       = qint64(o.value(QStringLiteral("clock")).toDouble(0.0));
    out.buffering     = o.value(QStringLiteral("buffering")).toBool(false);
    out.reason        = o.value(QStringLiteral("why")).toString();
    const QJsonObject ro = o.value(QStringLiteral("ref")).toObject();
    out.ref.kind   = ro.value(QStringLiteral("kind")).toString();
    out.ref.id     = ro.value(QStringLiteral("id")).toString();
    out.ref.type   = ro.value(QStringLiteral("type")).toString();
    out.ref.title  = ro.value(QStringLiteral("title")).toString();
    out.ref.source = ro.value(QStringLiteral("source")).toString();
    const QJsonArray arr = o.value(QStringLiteral("roster")).toArray();
    for (const QJsonValue& v : arr)
    {
        const QJsonObject eo = v.toObject();
        RosterEntry e;
        e.id        = eo.value(QStringLiteral("id")).toString();
        e.name      = eo.value(QStringLiteral("name")).toString();
        e.host      = eo.value(QStringLiteral("host")).toBool(false);
        e.buffering = eo.value(QStringLiteral("buffering")).toBool(false);
        e.resolved  = eo.value(QStringLiteral("resolved")).toBool(true);
        out.roster << e;
    }
    return true;
}

QList<Message> decodeStream(QByteArray& buf)
{
    QList<Message> out;
    for (;;)
    {
        const int nl = buf.indexOf('\n');
        if (nl < 0) break;
        const QByteArray line = buf.left(nl);
        buf.remove(0, nl + 1);
        if (line.trimmed().isEmpty()) continue;
        Message m;
        QString err;
        // A line that does not parse is DROPPED. One malformed control message must not end a watch party,
        // and there is nothing here whose loss cannot be repaired by the next beacon.
        if (decode(line, m, err)) out << m;
    }
    return out;
}

// ---- 4. the drift decision -------------------------------------------------------------------------------

DriftDecision decideDrift(double hostPos, bool hostPaused, double localPos, double beaconAgeSec,
                          bool nudging, const DriftConfig& cfg)
{
    DriftDecision d;
    // A beacon from the future, a NaN age, or one older than the sanity window is not evidence. The `!(>= 0)`
    // spelling is deliberate: it rejects NaN, which `< 0` would let through.
    if (!(beaconAgeSec >= 0.0) || beaconAgeSec > cfg.maxBeaconAgeSec) return d;
    if (!std::isfinite(hostPos) || !std::isfinite(localPos)) return d;

    // Where the host is NOW, not where it was when it spoke. A paused host does not advance.
    const double target = hostPaused ? hostPos : hostPos + beaconAgeSec;
    d.drift = localPos - target;
    const double mag = std::fabs(d.drift);

    if (mag >= cfg.hardSeekSec)
    {
        d.action = DriftAction::HardSeek;
        d.seekTo = target < 0.0 ? 0.0 : target;
        d.speed  = 1.0;
        return d;
    }
    // Two thresholds, and this is the line that makes the controller stable: entering a correction costs
    // `deadband`, leaving it costs only `release`. Exactly AT either threshold is "do nothing" — a boundary
    // has to belong to one side, and giving it to the quiet side is what stops a drift that sits on the
    // deadband from arming and disarming on alternate beacons.
    const double threshold = nudging ? cfg.releaseSec : cfg.deadbandSec;
    if (mag <= threshold) return d;

    // Proportional, clamped. Ahead of the host (drift > 0) means slow down.
    d.action = DriftAction::Nudge;
    d.speed  = clampd(1.0 - d.drift / cfg.correctionSec, 1.0 - cfg.maxNudge, 1.0 + cfg.maxNudge);
    return d;
}

// ---- 5. buffering ----------------------------------------------------------------------------------------

QString policyId(BufferPolicy p)
{
    return p == BufferPolicy::KeepGoing ? QStringLiteral("keepgoing") : QStringLiteral("wait");
}

BufferPolicy policyFromId(const QString& id)
{
    return id == QLatin1String("keepgoing") ? BufferPolicy::KeepGoing : BufferPolicy::WaitForEveryone;
}

BufferAction decideBuffering(BufferPolicy policy, bool anyoneBuffering, bool hostPlaying,
                             bool heldForBuffering)
{
    if (policy == BufferPolicy::KeepGoing)
    {
        // Switching the policy to "keep going" WHILE the room is held has to release it. Otherwise the room
        // sits paused for a stall the user has just said they do not care about, and nothing would ever
        // resume it.
        return heldForBuffering ? BufferAction::ResumeAfterBuffering : BufferAction::Nothing;
    }
    if (anyoneBuffering && hostPlaying && !heldForBuffering) return BufferAction::HoldForBuffering;
    if (!anyoneBuffering && heldForBuffering) return BufferAction::ResumeAfterBuffering;
    return BufferAction::Nothing;
}

// ---- 6. the room -----------------------------------------------------------------------------------------

void Room::open(const QString& code, const QString& selfId, const QString& selfName, bool asHost)
{
    active_ = true;
    host_ = asHost;
    code_ = normalizeCode(code);
    selfId_ = selfId;
    selfName_ = selfName;
    people_.clear();
    Participant me;
    me.id = selfId;
    me.name = selfName;
    me.host = asHost;
    people_ << me;
    item_ = PlayOn::ItemRef();
    hostPos_ = 0.0;
    hostPaused_ = true;
    hostClockMs_ = 0;
    held_ = false;
}

void Room::close()
{
    active_ = false;
    people_.clear();
    item_ = PlayOn::ItemRef();
    held_ = false;
}

int Room::indexOf(const QString& id) const
{
    for (int i = 0; i < people_.size(); ++i)
        if (people_.at(i).id == id) return i;
    return -1;
}

Participant* Room::findMut(const QString& id)
{
    const int i = indexOf(id);
    return i < 0 ? nullptr : &people_[i];
}

bool Room::anyoneBuffering() const
{
    // An UNRESOLVED participant cannot hold the room: they are not watching, so their "buffering" flag (which
    // is stale by definition once they gave up) must not pause everyone else.
    for (const Participant& p : people_)
        if (p.buffering && p.resolved) return true;
    return false;
}

void Room::setItem(const PlayOn::ItemRef& ref, double positionSec, bool paused)
{
    item_ = shareableRef(ref);   // scrubbed on the way IN as well, so nothing unsafe is ever even held
    hostPos_ = positionSec;
    hostPaused_ = paused;
}

void Room::setHostTransport(bool paused, double positionSec)
{
    hostPaused_ = paused;
    hostPos_ = positionSec;
    held_ = false;   // a human decision replaces the automatic hold
}

void Room::noteHostPosition(double positionSec) { hostPos_ = positionSec; }

bool Room::setSelfUnresolved(const QString& why)
{
    Participant* me = findMut(selfId_);
    if (!me) return false;
    const bool changed = me->resolved || me->note != scrubForWire(why);
    me->resolved = false;
    me->note = scrubForWire(why);
    me->buffering = false;
    return changed;
}

bool Room::setSelfResolved()
{
    Participant* me = findMut(selfId_);
    if (!me || me->resolved) return false;    // already resolved: saying so again is the loop, not the news
    me->resolved = true;
    me->note.clear();
    return true;
}

bool Room::selfResolved() const
{
    const int i = indexOf(selfId_);
    return i < 0 || people_.at(i).resolved;
}

Message Room::helloMessage() const
{
    Message m;
    m.type = MsgType::Hello;
    m.participantId = selfId_;
    m.displayName = selfName_;
    m.host = host_;
    return m;
}

Message Room::rosterMessage() const
{
    Message m;
    m.type = MsgType::Roster;
    m.participantId = selfId_;
    for (const Participant& p : people_)
    {
        RosterEntry e;
        e.id = p.id; e.name = p.name; e.host = p.host; e.buffering = p.buffering; e.resolved = p.resolved;
        m.roster << e;
    }
    return m;
}

Message Room::itemMessage() const
{
    Message m;
    m.type = MsgType::Item;
    m.participantId = selfId_;
    m.ref = item_;
    m.positionSec = hostPos_;
    m.paused = hostPaused_;
    return m;
}

Message Room::transportMessage() const
{
    Message m;
    m.type = MsgType::Transport;
    m.participantId = selfId_;
    m.positionSec = hostPos_;
    m.paused = hostPaused_;
    return m;
}

Message Room::beaconMessage(qint64 clockMs)
{
    hostClockMs_ = clockMs;
    Message m;
    m.type = MsgType::Beacon;
    m.participantId = selfId_;
    m.positionSec = hostPos_;
    m.paused = hostPaused_;
    m.clockMs = clockMs;
    return m;
}

Message Room::requestPauseMessage(bool paused) const
{
    Message m;
    m.type = MsgType::RequestPause;
    m.participantId = selfId_;
    m.paused = paused;
    return m;
}

Message Room::requestSeekMessage(double positionSec) const
{
    Message m;
    m.type = MsgType::RequestSeek;
    m.participantId = selfId_;
    m.positionSec = positionSec < 0.0 ? 0.0 : positionSec;
    return m;
}

Message Room::bufferingMessage(bool buffering, double positionSec) const
{
    Message m;
    m.type = MsgType::Buffering;
    m.participantId = selfId_;
    m.buffering = buffering;
    m.positionSec = positionSec;
    return m;
}

Message Room::unresolvedMessage(const QString& why) const
{
    Message m;
    m.type = MsgType::Unresolved;
    m.participantId = selfId_;
    m.reason = why;
    return m;
}

Message Room::byeMessage() const
{
    Message m;
    m.type = MsgType::Bye;
    m.participantId = selfId_;
    // Whether the HOST is the one leaving is the difference between "someone left the party" and "the party
    // is over", so it rides on the message rather than being inferred from an id the receiver may not hold.
    m.host = host_;
    return m;
}

QList<Message> Room::hostBufferingSweep()
{
    const BufferAction a = decideBuffering(policy_, anyoneBuffering(), !hostPaused_, held_);
    if (a == BufferAction::HoldForBuffering)  { hostPaused_ = true;  held_ = true;  return { transportMessage() }; }
    if (a == BufferAction::ResumeAfterBuffering) { hostPaused_ = false; held_ = false; return { transportMessage() }; }
    return {};
}

QList<Message> Room::apply(const Message& in)
{
    QList<Message> out;
    if (!active_) return out;

    if (!host_)
    {
        // ---- guest: the host's state is the truth; nothing here answers back.
        switch (in.type)
        {
            case MsgType::Item:
                item_ = shareableRef(in.ref);
                hostPos_ = in.positionSec;
                hostPaused_ = in.paused;
                break;
            case MsgType::Transport:
                hostPos_ = in.positionSec;
                hostPaused_ = in.paused;
                break;
            case MsgType::Beacon:
                hostPos_ = in.positionSec;
                hostPaused_ = in.paused;
                hostClockMs_ = in.clockMs;
                break;
            case MsgType::Roster:
            {
                // The host owns membership. Rebuild wholesale, but keep OUR OWN note: the reason we could not
                // resolve is a local fact and the roster does not carry it.
                QString myNote;
                if (const int i = indexOf(selfId_); i >= 0) myNote = people_.at(i).note;
                people_.clear();
                for (const RosterEntry& e : in.roster)
                {
                    Participant p;
                    p.id = e.id; p.name = e.name; p.host = e.host;
                    p.buffering = e.buffering; p.resolved = e.resolved;
                    if (p.id == selfId_) p.note = myNote;
                    people_ << p;
                }
                break;
            }
            case MsgType::Bye:
                if (const int i = indexOf(in.participantId); i >= 0) people_.removeAt(i);
                break;
            default:
                break;
        }
        return out;
    }

    // ---- host: the arbiter.
    switch (in.type)
    {
        case MsgType::Hello:
        {
            if (in.protocol != kProtocol)
            {
                Message no = byeMessage();
                no.reason = QStringLiteral("that copy of the app speaks a different watch-together version");
                out << no;
                return out;
            }
            // A REJOIN is the same id arriving again: refresh it in place rather than adding a second row.
            if (Participant* p = findMut(in.participantId))
            {
                p->name = in.displayName;
                p->buffering = false;
                p->resolved = true;
                p->note.clear();
            }
            else
            {
                Participant fresh;
                fresh.id = in.participantId;
                fresh.name = in.displayName;
                people_ << fresh;
            }
            // Join in progress, in this order and deliberately: WHAT before WHERE. A guest that learned the
            // position first would seek a player holding the previous item.
            out << rosterMessage() << itemMessage() << transportMessage();
            out << hostBufferingSweep();
            return out;
        }
        case MsgType::Buffering:
        {
            Participant* p = findMut(in.participantId);
            if (!p) return out;
            p->buffering = in.buffering;
            p->positionSec = in.positionSec;
            out << rosterMessage();
            out << hostBufferingSweep();
            return out;
        }
        case MsgType::Unresolved:
        {
            Participant* p = findMut(in.participantId);
            if (!p) return out;
            // They stay in the room. They are simply not watching, and cannot hold it up or steer it.
            p->resolved = false;
            p->note = scrubForWire(in.reason);
            p->buffering = false;
            out << rosterMessage();
            out << hostBufferingSweep();
            return out;
        }
        case MsgType::RequestPause:
        case MsgType::RequestSeek:
        {
            const Participant* p = nullptr;
            if (const int i = indexOf(in.participantId); i >= 0) p = &people_.at(i);
            // Not in the room, or in it but unable to play the item: refused. Someone who could not get the
            // film does not get to pause it for the people who did.
            if (!p || !p->resolved) return out;
            if (in.type == MsgType::RequestPause) setHostTransport(in.paused, hostPos_);
            else                                  setHostTransport(hostPaused_, in.positionSec < 0.0 ? 0.0 : in.positionSec);
            out << transportMessage();
            return out;
        }
        case MsgType::Bye:
        {
            if (const int i = indexOf(in.participantId); i >= 0) people_.removeAt(i);
            out << rosterMessage();
            out << hostBufferingSweep();
            return out;
        }
        default:
            return out;   // a guest does not send Item/Transport/Beacon/Roster; if one does, it is ignored
    }
}

// ---- 7. the persisted record -----------------------------------------------------------------------------

QVariantMap roomRecord(const Room& r)
{
    const PlayOn::ItemRef ref = shareableRef(r.item());
    QVariantMap m;
    m.insert(QStringLiteral("code"), r.code());
    m.insert(QStringLiteral("role"), r.isHost() ? QStringLiteral("host") : QStringLiteral("guest"));
    m.insert(QStringLiteral("refKind"), ref.kind);
    m.insert(QStringLiteral("refId"), ref.id);
    m.insert(QStringLiteral("refType"), ref.type);
    m.insert(QStringLiteral("refTitle"), ref.title);
    m.insert(QStringLiteral("refSource"), ref.source);
    m.insert(QStringLiteral("policy"), policyId(r.bufferPolicy()));
    return m;
}

bool recordSafe(const QVariantMap& record)
{
    for (auto it = record.constBegin(); it != record.constEnd(); ++it)
        if (looksLikeCredential(it.value().toString())) return false;
    return true;
}

}   // namespace WatchTogether
