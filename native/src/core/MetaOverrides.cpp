#include "MetaOverrides.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QSettings>

// Shares the portable everythingbox.ini with the other per-item stores (same AppPaths::dataDir() posture).
// Coherence with any other QSettings on the same file comes from every writer calling sync().
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

namespace {

using MetaOverrides::Override;

// Change-callback (mdsync T2 contract): fired after every mutation to (re)arm the debounced Drive push.
std::function<void()> g_changeHook;
void fireChanged() { if (g_changeHook) g_changeHook(); }

const QLatin1String kItemsGroup("metaoverrides/items");

QString itemKey(const QString& hash) { return kItemsGroup + QLatin1Char('/') + hash; }

// ---- lazy cache -------------------------------------------------------------------------------------------
// get() is on the browse hot path (one call per catalog tile), so the group resolution + blob parse happen
// once per build rather than per call. No profile dimension to self-heal: the store is global.
bool                     mCacheBuilt = false;
QHash<QString, Override> mCache;      // itemHash -> Override (husks are NOT cached; they read as absent)

void ensureCache()
{
    if (mCacheBuilt) return;
    mCache.clear();
    QSettings& s = store();
    s.beginGroup(kItemsGroup);
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
    {
        const Override ov = MetaOverrides::fromJson(
            QJsonDocument::fromJson(s.value(h).toString().toUtf8()).object());
        if (ov.isEmpty()) continue;  // a reset husk: present for the merge, but nothing to composite
        mCache.insert(h, ov);
    }
    s.endGroup();
    mCacheBuilt = true;
}

} // namespace

bool Override::isEmpty() const
{
    return title.isEmpty() && subtitle.isEmpty() && overview.isEmpty() && image.isEmpty();
}

// ---- pure: canonical record <-> JSON ----------------------------------------------------------------------

Override MetaOverrides::fromJson(const QJsonObject& o)
{
    Override ov;
    ov.title     = o.value(QStringLiteral("title")).toString();
    ov.subtitle  = o.value(QStringLiteral("subtitle")).toString();
    ov.overview  = o.value(QStringLiteral("overview")).toString();
    ov.image     = o.value(QStringLiteral("image")).toString();
    ov.updatedAt = static_cast<qint64>(o.value(QStringLiteral("updatedAt")).toDouble());
    return ov;
}

Override MetaOverrides::normalized(const Override& ov)
{
    Override n;
    n.title     = ov.title.trimmed();
    n.subtitle  = ov.subtitle.trimmed();
    n.overview  = ov.overview.trimmed();
    n.image     = ov.image.trimmed();
    n.updatedAt = ov.updatedAt;
    return n;
}

QJsonObject MetaOverrides::toJson(const Override& in)
{
    // ONE canonical spelling: trimmed, and an unset field is ABSENT (never ""). Two devices that made the same
    // correction therefore produce byte-identical records, so CloudMerge's equal-timestamp tie-break sees no
    // difference and neither device flips the other's copy.
    const Override ov = normalized(in);
    QJsonObject o;
    if (!ov.title.isEmpty())    o.insert(QStringLiteral("title"), ov.title);
    if (!ov.subtitle.isEmpty()) o.insert(QStringLiteral("subtitle"), ov.subtitle);
    if (!ov.overview.isEmpty()) o.insert(QStringLiteral("overview"), ov.overview);
    if (!ov.image.isEmpty())    o.insert(QStringLiteral("image"), ov.image);
    o.insert(QStringLiteral("updatedAt"), static_cast<double>(ov.updatedAt));
    return o;
}

// ---- pure: the composite rule -------------------------------------------------------------------------------

QString MetaOverrides::pick(const QString& override, const QString& scraped)
{
    return override.isEmpty() ? scraped : override;
}

void MetaOverrides::applyTo(const Override& ov, MediaDetail& d)
{
    d.title    = pick(ov.title, d.title);
    d.subtitle = pick(ov.subtitle, d.subtitle);
    d.overview = pick(ov.overview, d.overview);
    d.imageUrl = pick(ov.image, d.imageUrl);
    applyTo(ov, d.art);
    // A card that had nothing scraped but DOES carry a correction is now showable — otherwise the one place
    // the user could fix a blank item would keep reporting itself as empty.
    if (!ov.isEmpty()) d.valid = d.valid || !d.title.isEmpty() || !d.art.isEmpty();
}

void MetaOverrides::applyTo(const Override& ov, MediaItem& it)
{
    it.title        = pick(ov.title, it.title);
    it.subtitle     = pick(ov.subtitle, it.subtitle);
    it.thumbnailUrl = pick(ov.image, it.thumbnailUrl);
    applyTo(ov, it.art);
}

void MetaOverrides::applyTo(const Override& ov, MediaArt& art)
{
    if (ov.image.isEmpty()) return;
    // Lead the two cover roles with the corrected image. Prepending rather than replacing keeps the scraped
    // candidates behind it, so a later "reset to scraped" has something to fall back to without a re-fetch —
    // and every theme binding (selected.poster / selected.thumb) picks up the correction, because those alias
    // to the role's FIRST candidate. This runs AFTER MetaCache::loadArt has put the locally cached file at the
    // front, which is the point: that cached file is the wrong art the user is correcting.
    for (const QString& role : { QStringLiteral("poster"), QStringLiteral("thumb") })
    {
        QStringList list = art.images.value(role);
        list.removeAll(ov.image);
        list.prepend(ov.image);
        art.images.insert(role, list);
    }
}

void MetaOverrides::applyTo(const Override& ov, QVariantMap& row)
{
    if (ov.isEmpty()) return;
    auto put = [&row](const QString& key, const QString& value) {
        if (!value.isEmpty()) row.insert(key, value);
    };
    put(QStringLiteral("title"), ov.title);
    put(QStringLiteral("subtitle"), ov.subtitle);
    put(QStringLiteral("overview"), ov.overview);
    if (ov.image.isEmpty()) return;
    row.insert(QStringLiteral("image"), ov.image);
    // The scalar role aliases MediaArt::writeInto emits (selected.poster / selected.thumb) and the images
    // sub-map galleries read. Written here rather than left to writeInto because a row can be assembled from
    // a session cache that never went near MediaArt on this pass.
    QVariantMap images = row.value(QStringLiteral("images")).toMap();
    for (const QString& role : { QStringLiteral("poster"), QStringLiteral("thumb") })
    {
        row.insert(role, ov.image);
        QStringList list = images.value(role).toStringList();
        list.removeAll(ov.image);
        list.prepend(ov.image);
        images.insert(role, list);
    }
    row.insert(QStringLiteral("images"), images);
}

// ---- store ---------------------------------------------------------------------------------------------------

QString MetaOverrides::hashKey(const QString& key)
{
    // MD5-hex over UTF-8 — ItemMarks' scheme over ItemMarks' key space (MetaCache::keyFor), so the same item
    // hashes the same way in both stores. Flattens urls/paths whose '/' QSettings would read as group nesting.
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

Override MetaOverrides::get(const QString& key)
{
    if (key.isEmpty()) return Override{};
    ensureCache();
    return mCache.value(hashKey(key));
}

bool MetaOverrides::has(const QString& key)
{
    return !get(key).isEmpty();
}

void MetaOverrides::set(const QString& key, const Override& in)
{
    if (key.isEmpty()) return;
    Override ov = normalized(in);
    ov.updatedAt = QDateTime::currentSecsSinceEpoch(); // the merge funnel: every content write bumps the stamp
    store().setValue(itemKey(hashKey(key)),
                     QString::fromUtf8(QJsonDocument(toJson(ov)).toJson(QJsonDocument::Compact)));
    store().sync();
    invalidate();
    fireChanged();
}

void MetaOverrides::reset(const QString& key)
{
    if (key.isEmpty()) return;
    // Deliberately NOT store().remove(): a deleted row is indistinguishable from "this device never knew about
    // that item", so the next merge with a peer still holding the old override would put it straight back. The
    // husk is a real record with a newer timestamp, so it wins the merge and carries the reset to every device.
    set(key, Override{});
}

int MetaOverrides::count()
{
    ensureCache();
    return mCache.size();   // husks are not cached, so a reset item is not counted
}

void MetaOverrides::clearAll()
{
    ensureCache();
    const QStringList hashes = mCache.keys();
    for (const QString& h : hashes)
        store().setValue(itemKey(h), QString::fromUtf8(QJsonDocument(toJson(Override{
            QString(), QString(), QString(), QString(), QDateTime::currentSecsSinceEpoch() }))
                                                           .toJson(QJsonDocument::Compact)));
    store().sync();
    invalidate();
    fireChanged();
}

void MetaOverrides::invalidate()
{
    mCacheBuilt = false;
    mCache.clear();
}

void MetaOverrides::setChangeHook(std::function<void()> hook)
{
    g_changeHook = std::move(hook);
}
