#include "AudioBookmarkStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "Tombstones.h"

#include <QSettings>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QtGlobal>
#include <algorithm>

// Shares the portable everythingbox.ini with the other per-item stores (same AppPaths::dataDir() posture).
// Coherence with any other QSettings on the same file comes from every writer calling sync(); QSettings reloads
// on access when the on-disk file changed.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Per-profile, so each viewer keeps their own bookmarks (annotations are per-viewer). The profile leaf mirrors
// BookmarkStore/FavoritesStore's ("default" when none is set), so the stores namespace identically.
static QString profileId()
{
    const QString id = ProfileStore::currentId();
    return id.isEmpty() ? QStringLiteral("default") : id;
}

QString AudioBookmarkStore::itemsKey()
{
    return QStringLiteral("audiobookmarks/") + profileId() + QStringLiteral("/items");
}

QString AudioBookmarkStore::tombstoneStore()
{
    return QStringLiteral("audiobookmarks/") + profileId();
}

// Change-callback: fired after a mutation to (re)arm the debounced Drive push; null in probes.
static std::function<void()> g_changeHook;
void AudioBookmarkStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }
static void fireChanged() { if (g_changeHook) g_changeHook(); }

QString AudioBookmarkStore::idFor(const QString& itemKey, double posSec)
{
    if (itemKey.isEmpty()) return QString();
    // md5(itemKey | whole-second). Rounding to a whole second makes the id a pure function of the POSITION at
    // the resolution a listener cares about: two devices that bookmark "the same spot" collapse to one id, and a
    // re-bookmark of that spot is idempotent. A negative/absurd position clamps at 0.
    const qint64 sec = qRound64(qMax(0.0, posSec));
    QByteArray seed = itemKey.toUtf8();
    seed.append('|');
    seed.append(QByteArray::number(sec));
    return QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Md5).toHex().left(16));
}

static AudioBookmarkStore::Bookmark fromObject(const QJsonObject& o)
{
    AudioBookmarkStore::Bookmark b;
    b.id      = o.value(QStringLiteral("id")).toString();
    b.itemKey = o.value(QStringLiteral("itemKey")).toString();
    b.posSec  = o.value(QStringLiteral("posSec")).toDouble();
    b.label   = o.value(QStringLiteral("label")).toString();
    b.ts      = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
    return b;
}

static QJsonObject toObject(const AudioBookmarkStore::Bookmark& b)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), b.id);
    o.insert(QStringLiteral("itemKey"), b.itemKey);
    o.insert(QStringLiteral("posSec"), b.posSec);
    o.insert(QStringLiteral("label"), b.label);
    o.insert(QStringLiteral("ts"), static_cast<double>(b.ts));
    return o;
}

// The whole active-profile list (all items), as stored.
static QVector<AudioBookmarkStore::Bookmark> readAll()
{
    QVector<AudioBookmarkStore::Bookmark> out;
    const QByteArray raw = store().value(AudioBookmarkStore::itemsKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(raw).array())
        if (v.isObject())
        {
            const AudioBookmarkStore::Bookmark b = fromObject(v.toObject());
            if (!b.id.isEmpty()) out.push_back(b);
        }
    return out;
}

static void writeAll(const QVector<AudioBookmarkStore::Bookmark>& items)
{
    QJsonArray arr;
    for (const AudioBookmarkStore::Bookmark& b : items) arr.append(toObject(b));
    store().setValue(AudioBookmarkStore::itemsKey(),
                     QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

AudioBookmarkStore::Bookmark AudioBookmarkStore::add(const QString& itemKey, double posSec, const QString& label)
{
    if (itemKey.isEmpty()) return Bookmark();

    Bookmark bm;
    bm.id      = idFor(itemKey, posSec);
    bm.itemKey = itemKey;
    bm.posSec  = posSec;
    bm.label   = label;
    bm.ts      = QDateTime::currentSecsSinceEpoch();

    QVector<Bookmark> items = readAll();
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].id == bm.id) items.remove(i);   // idempotent by position: fold onto the one id
    items.push_back(bm);
    writeAll(items);

    // A re-add of a previously-removed spot resurrects it: clear the stale tombstone so it does not
    // self-suppress on the next merge. The fresh ts would beat an older tombstone anyway, but clearing keeps
    // this device's outbound document honest.
    Tombstones::remove(tombstoneStore(), bm.id);

    fireChanged();
    return bm;
}

void AudioBookmarkStore::remove(const QString& id)
{
    if (id.isEmpty()) return;
    QVector<Bookmark> items = readAll();
    bool removed = false;
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].id == id) { items.remove(i); removed = true; }
    if (!removed) return;
    writeAll(items);
    // Tombstone the removed id so a peer that still holds it cannot resurrect it on merge.
    Tombstones::record(tombstoneStore(), id);
    fireChanged();
}

QVector<AudioBookmarkStore::Bookmark> AudioBookmarkStore::list(const QString& itemKey)
{
    QVector<Bookmark> out;
    if (itemKey.isEmpty()) return out;
    for (const Bookmark& b : readAll())
        if (b.itemKey == itemKey) out.push_back(b);
    std::sort(out.begin(), out.end(), [](const Bookmark& a, const Bookmark& b) {
        return a.posSec < b.posSec;   // playback order
    });
    return out;
}

QVector<AudioBookmarkStore::Bookmark> AudioBookmarkStore::all() { return readAll(); }
