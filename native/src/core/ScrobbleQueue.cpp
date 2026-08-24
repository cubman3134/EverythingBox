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
