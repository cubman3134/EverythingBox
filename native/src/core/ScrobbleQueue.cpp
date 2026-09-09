#include "ScrobbleQueue.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

// Shares the portable everythingbox.ini with the other per-profile stores (MissedDismiss.cpp's posture).
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

namespace {

std::function<void()> g_changeHook;
void fireChanged() { if (g_changeHook) g_changeHook(); }

// "scrobblestate/<profile>/<provider>/" — one spelling, used by every key builder below, so the four keys can
// never end up in different profiles' groups.
QString groupFor(const QString& profileId, const QString& providerId)
{
    return Scrobble::stateKeyPrefix() + Scrobble::profileSlot(profileId) + QStringLiteral("/")
         + (providerId.isEmpty() ? QStringLiteral("unknown") : providerId) + QStringLiteral("/");
}

QString kindToken(Scrobble::Kind k) { return k == Scrobble::Kind::Spoken ? QStringLiteral("spoken")
                                                                        : QStringLiteral("music"); }
Scrobble::Kind kindFromToken(const QString& t)
{
    return t == QLatin1String("spoken") ? Scrobble::Kind::Spoken : Scrobble::Kind::Music;
}

QString originToken(Scrobble::Origin o)
{
    switch (o)
    {
        case Scrobble::Origin::Remote: return QStringLiteral("remote");
        case Scrobble::Origin::Server: return QStringLiteral("server");
        case Scrobble::Origin::LocalLibrary: break;
    }
    return QStringLiteral("local");
}
Scrobble::Origin originFromToken(const QString& t)
{
    if (t == QLatin1String("remote")) return Scrobble::Origin::Remote;
    if (t == QLatin1String("server")) return Scrobble::Origin::Server;
    return Scrobble::Origin::LocalLibrary;
}

} // namespace

void ScrobbleQueue::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }

QString ScrobbleQueue::queueKey(const QString& profileId, const QString& providerId)
{ return groupFor(profileId, providerId) + QStringLiteral("queue"); }
QString ScrobbleQueue::counterKey(const QString& profileId, const QString& providerId)
{ return groupFor(profileId, providerId) + QStringLiteral("count"); }
QString ScrobbleQueue::droppedKey(const QString& profileId, const QString& providerId)
{ return groupFor(profileId, providerId) + QStringLiteral("dropped"); }
QString ScrobbleQueue::errorKey(const QString& profileId, const QString& providerId)
{ return groupFor(profileId, providerId) + QStringLiteral("error"); }

// The wire form is deliberately SHORT-KEYED. This list lives in an ini row that a long offline stretch can
// fill with thousands of entries; "a"/"t"/"b" instead of "artist"/"title"/"album" is most of the difference
// between a row that is tens of kilobytes and one that is hundreds. Absent fields are omitted entirely, so an
// ordinary tagged track with no album artist and no track number costs four keys.
QByteArray ScrobbleQueue::encode(const QVector<Scrobble::Play>& plays)
{
    QJsonArray arr;
    for (const Scrobble::Play& p : plays)
    {
        QJsonObject o;
        o.insert(QStringLiteral("ts"), double(p.listenedAt));
        o.insert(QStringLiteral("a"), p.track.artist);
        o.insert(QStringLiteral("t"), p.track.title);
        if (!p.track.album.isEmpty())       o.insert(QStringLiteral("b"), p.track.album);
        if (!p.track.albumArtist.isEmpty()) o.insert(QStringLiteral("aa"), p.track.albumArtist);
        if (p.track.trackNumber > 0)        o.insert(QStringLiteral("n"), p.track.trackNumber);
        if (p.track.durationSec > 0)        o.insert(QStringLiteral("d"), p.track.durationSec);
        if (p.track.kind != Scrobble::Kind::Music)
            o.insert(QStringLiteral("k"), kindToken(p.track.kind));
        if (p.track.origin != Scrobble::Origin::LocalLibrary)
            o.insert(QStringLiteral("o"), originToken(p.track.origin));
        // THE SUPPLIER'S OWN ID (#193 increment 6), when there is one. It has to survive a night on disk or a
        // music server can never be told about a listen it was offline for: `scrobble.view` takes the id and
        // nothing else, so a row that arrives without it is a row that can never be delivered. Absent for
        // every local and addon play, which is the ordinary case and costs nothing.
        //
        // It is not a credential — Scrobble.h says so where the field is declared, and the reason the point
        // is worth making is that the LAZY way to carry the same fact would have been the signed stream url,
        // which would have written the user's token into this file.
        if (!p.track.sourceId.isEmpty()) o.insert(QStringLiteral("sid"), p.track.sourceId);
        arr.append(o);
    }
    return QJsonDocument(arr).toJson(QJsonDocument::Compact);
}

QVector<Scrobble::Play> ScrobbleQueue::decode(const QByteArray& json)
{
    QVector<Scrobble::Play> out;
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    out.reserve(arr.size());
    for (const QJsonValue& v : arr)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        Scrobble::Play p;
        p.listenedAt          = qint64(o.value(QStringLiteral("ts")).toDouble());
        p.track.artist        = o.value(QStringLiteral("a")).toString();
        p.track.title         = o.value(QStringLiteral("t")).toString();
        p.track.album         = o.value(QStringLiteral("b")).toString();
        p.track.albumArtist   = o.value(QStringLiteral("aa")).toString();
        p.track.trackNumber   = o.value(QStringLiteral("n")).toInt();
        p.track.durationSec   = o.value(QStringLiteral("d")).toInt();
        p.track.kind          = kindFromToken(o.value(QStringLiteral("k")).toString());
        p.track.origin        = originFromToken(o.value(QStringLiteral("o")).toString());
        p.track.sourceId      = o.value(QStringLiteral("sid")).toString();
        // A row with no timestamp cannot be backdated and would land at "now" — which is the one outcome this
        // whole file exists to prevent. Drop it rather than deliver a lie.
        if (p.listenedAt <= 0) continue;
        out.push_back(p);
    }
    return out;
}

int ScrobbleQueue::applyCap(QVector<Scrobble::Play>& plays)
{
    if (plays.size() <= kMaxQueued) return 0;
    const int over = int(plays.size()) - kMaxQueued;
    plays.remove(0, over);   // from the FRONT: the oldest listening is what a full queue gives up
    return over;
}

static QVector<Scrobble::Play> loadQueue(const QString& providerId)
{
    return ScrobbleQueue::decode(
        store().value(ScrobbleQueue::queueKey(ProfileStore::currentId(), providerId)).toString().toUtf8());
}

static void saveQueue(const QString& providerId, const QVector<Scrobble::Play>& plays)
{
    const QString key = ScrobbleQueue::queueKey(ProfileStore::currentId(), providerId);
    if (plays.isEmpty()) store().remove(key);
    else                 store().setValue(key, QString::fromUtf8(ScrobbleQueue::encode(plays)));
    store().sync();
}

void ScrobbleQueue::append(const QString& providerId, const Scrobble::Play& play)
{
    if (providerId.isEmpty() || play.listenedAt <= 0) return;
    QVector<Scrobble::Play> plays = loadQueue(providerId);
    plays.push_back(play);
    const int lost = applyCap(plays);
    if (lost > 0)
    {
        const QString dk = droppedKey(ProfileStore::currentId(), providerId);
        store().setValue(dk, store().value(dk).toInt() + lost);
    }
    saveQueue(providerId, plays);
    fireChanged();
}

QVector<Scrobble::Play> ScrobbleQueue::head(const QString& providerId, int n)
{
    if (n <= 0) return {};
    QVector<Scrobble::Play> plays = loadQueue(providerId);
    if (plays.size() > n) plays.resize(n);
    return plays;
}

void ScrobbleQueue::dropFront(const QString& providerId, int n)
{
    if (n <= 0) return;
    QVector<Scrobble::Play> plays = loadQueue(providerId);
    if (plays.isEmpty()) return;
    plays.remove(0, qMin(n, int(plays.size())));
    saveQueue(providerId, plays);
    fireChanged();
}

int ScrobbleQueue::count(const QString& providerId) { return int(loadQueue(providerId).size()); }

void ScrobbleQueue::clear(const QString& providerId)
{
    saveQueue(providerId, {});
    fireChanged();
}

// Read back the ids out of the KEYS, because nothing else knows them. The group is written by exactly one
// spelling (groupFor above), so the id is whatever sits between this profile's group and the leaf key —
// taken with lastIndexOf so a leaf can never be mistaken for part of an id.
QStringList ScrobbleQueue::providerIdsOnDisk()
{
    // Another QSettings object on the same file may have written since this one last read. Cheap, and the
    // sweep runs once a launch.
    store().sync();
    const QString prefix = groupFor(ProfileStore::currentId(), QString());
    // groupFor() substitutes "unknown" for an empty provider, so trim that placeholder back off to get the
    // bare "<state>/<profile>/" this walk needs. Built through groupFor on purpose: a second spelling of the
    // profile carve-out here is a second thing to keep in step with Scrobble::stateKeyPrefix().
    const QString base = prefix.left(prefix.size() - QStringLiteral("unknown/").size());
    QStringList out;
    for (const QString& key : store().allKeys())
    {
        if (!key.startsWith(base)) continue;
        const QString rest = key.mid(base.size());
        const int cut = rest.lastIndexOf(QLatin1Char('/'));
        if (cut <= 0) continue;                       // a stray key with no provider group: not ours
        const QString pid = rest.left(cut);
        if (!pid.isEmpty() && !out.contains(pid)) out.push_back(pid);
    }
    return out;
}

// The one operation that destroys unsent listening history. See the header for the two callers it has and
// for the rule both of them obey.
void ScrobbleQueue::forget(const QString& providerId)
{
    if (providerId.isEmpty()) return;
    const QString profile = ProfileStore::currentId();
    store().remove(queueKey(profile, providerId));
    store().remove(counterKey(profile, providerId));
    store().remove(droppedKey(profile, providerId));
    store().remove(errorKey(profile, providerId));
    store().sync();
    fireChanged();
}

int ScrobbleQueue::dropped(const QString& providerId)
{ return store().value(droppedKey(ProfileStore::currentId(), providerId)).toInt(); }

int ScrobbleQueue::delivered(const QString& providerId)
{ return store().value(counterKey(ProfileStore::currentId(), providerId)).toInt(); }

void ScrobbleQueue::noteDelivered(const QString& providerId, int n)
{
    if (n <= 0) return;
    const QString key = counterKey(ProfileStore::currentId(), providerId);
    store().setValue(key, store().value(key).toInt() + n);
    store().sync();
    fireChanged();
}

QString ScrobbleQueue::lastError(const QString& providerId)
{ return store().value(errorKey(ProfileStore::currentId(), providerId)).toString(); }

void ScrobbleQueue::setLastError(const QString& providerId, const QString& message)
{
    const QString key = errorKey(ProfileStore::currentId(), providerId);
    if (message.isEmpty()) store().remove(key);
    else                   store().setValue(key, message);
    store().sync();
}
