#include "BookmarkStore.h"
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

// Shares the portable everythingbox.ini with the other per-item stores (same AppPaths::dataDir() posture).
// Coherence with any other QSettings on the same file comes from every writer calling sync(); QSettings
// reloads on access when the on-disk file changed.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Per-profile, so each viewer keeps their own bookmarks (annotations are per-viewer). The profile leaf mirrors
// FavoritesStore's ("default" when none is set), so the two stores namespace identically.
static QString profileId()
{
    const QString id = ProfileStore::currentId();
    return id.isEmpty() ? QStringLiteral("default") : id;
}

QString BookmarkStore::itemsKey()
{
    return QStringLiteral("bookmarks/") + profileId() + QStringLiteral("/items");
}

QString BookmarkStore::tombstoneStore()
{
    return QStringLiteral("bookmarks/") + profileId();
}

// Change-callback (mdsync T2): fired after a mutation to (re)arm the debounced Drive push; null in probes.
static std::function<void()> g_changeHook;
void BookmarkStore::setChangeHook(std::function<void()> hook) { g_changeHook = std::move(hook); }
static void fireChanged() { if (g_changeHook) g_changeHook(); }

QString BookmarkStore::idFor(const QString& bookKey, const ReaderAnchor& anchor)
{
    if (bookKey.isEmpty()) return QString();
    // md5(bookKey | canonical-anchor). The anchor is canonicalised through its own toJson (QJsonObject stores
    // keys sorted, so Compact bytes are stable and device-independent), so the id is a pure function of the
    // POSITION — identical spots collapse to one id on every device.
    const QByteArray canon = QJsonDocument(anchor.toJson()).toJson(QJsonDocument::Compact);
    QByteArray seed = bookKey.toUtf8();
    seed.append('|');
    seed.append(canon);
    return QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Md5).toHex().left(16));
}

static ReaderAnchor anchorFromValue(const QJsonValue& v)
{
    return ReaderAnchor::fromJson(v.toObject());
}

static BookmarkStore::Bookmark bookmarkFromObject(const QJsonObject& o)
{
    BookmarkStore::Bookmark b;
    b.id      = o.value(QStringLiteral("id")).toString();
    b.bookKey = o.value(QStringLiteral("bookKey")).toString();
    b.anchor  = anchorFromValue(o.value(QStringLiteral("anchor")));
    b.label   = o.value(QStringLiteral("label")).toString();
    b.ts      = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
    return b;
}

static QJsonObject bookmarkToObject(const BookmarkStore::Bookmark& b)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), b.id);
    o.insert(QStringLiteral("bookKey"), b.bookKey);
    o.insert(QStringLiteral("anchor"), b.anchor.toJson());
    o.insert(QStringLiteral("label"), b.label);
    o.insert(QStringLiteral("ts"), static_cast<double>(b.ts));
    return o;
}

// The whole active-profile list (all books), as stored.
static QVector<BookmarkStore::Bookmark> readAll()
{
    QVector<BookmarkStore::Bookmark> out;
    const QByteArray raw = store().value(BookmarkStore::itemsKey()).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(raw).array())
        if (v.isObject())
        {
            const BookmarkStore::Bookmark b = bookmarkFromObject(v.toObject());
            if (!b.id.isEmpty()) out.push_back(b);
        }
    return out;
}

static void writeAll(const QVector<BookmarkStore::Bookmark>& items)
{
    QJsonArray arr;
    for (const BookmarkStore::Bookmark& b : items) arr.append(bookmarkToObject(b));
    store().setValue(BookmarkStore::itemsKey(), QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

BookmarkStore::Bookmark BookmarkStore::add(const QString& bookKey, const ReaderAnchor& anchor, const QString& label)
{
    if (bookKey.isEmpty()) return Bookmark();

    Bookmark bm;
    bm.id      = idFor(bookKey, anchor);
    bm.bookKey = bookKey;
    bm.anchor  = anchor;
    bm.label   = label;
    bm.ts      = QDateTime::currentSecsSinceEpoch();

    QVector<Bookmark> items = readAll();
    for (int i = items.size() - 1; i >= 0; --i)
        if (items[i].id == bm.id) items.remove(i);   // idempotent by position: fold onto the one id
    items.push_back(bm);
    writeAll(items);

    // A re-add of a previously-removed spot resurrects it: clear the stale tombstone so it does not
    // self-suppress on the next merge (the Tombstones "deletion undone" path). The fresh ts would beat an
    // older tombstone anyway, but clearing keeps this device's outbound document honest.
    Tombstones::remove(tombstoneStore(), bm.id);

    fireChanged();
    return bm;
}

void BookmarkStore::remove(const QString& id)
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

QVector<BookmarkStore::Bookmark> BookmarkStore::list(const QString& bookKey)
{
    QVector<Bookmark> out;
    if (bookKey.isEmpty()) return out;
    for (const Bookmark& b : readAll())
        if (b.bookKey == bookKey) out.push_back(b);
    std::sort(out.begin(), out.end(), [](const Bookmark& a, const Bookmark& b) {
        return ReaderAnchor::inReadingOrder(a.anchor, b.anchor);
    });
    return out;
}

QVector<BookmarkStore::Bookmark> BookmarkStore::all() { return readAll(); }
