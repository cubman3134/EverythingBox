#include "AudiobookMatchStore.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QSettings>

// Shares the portable everythingbox.ini with the other per-item stores (the AppPaths::dataDir() posture
// MetaOverrides states). Coherence with any other QSettings on the same file comes from every writer
// calling sync().
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

namespace {

using AudiobookMeta::Match;

std::function<void()> g_changeHook;
void fireChanged() { if (g_changeHook) g_changeHook(); }

const QLatin1String kItemsGroup("audiobookmatches/items");

QString itemKey(const QString& hash) { return kItemsGroup + QLatin1Char('/') + hash; }

// ---- lazy cache -----------------------------------------------------------------------------------------
// forBooks() runs once per index rebuild over every book in the library, so the group resolution and the
// blob parses happen once per process rather than once per book. REJECTIONS ARE CACHED TOO — unlike
// MetaOverrides, whose husks read as absent, a rejection here is a live answer the sweep has to see.
bool                  gCacheBuilt = false;
QHash<QString, Match> gCache;   // itemHash -> Match

void ensureCache()
{
    if (gCacheBuilt) return;
    gCache.clear();
    QSettings& s = store();
    s.beginGroup(kItemsGroup);
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
    {
        const Match m = AudiobookMeta::fromJson(
            QJsonDocument::fromJson(s.value(h).toString().toUtf8()).object());
        if (m.isEmpty() && !m.rejected) continue;   // an unreadable / empty row is not a record
        gCache.insert(h, m);
    }
    s.endGroup();
    gCacheBuilt = true;
}

void writeRecord(const QString& hash, const Match& m)
{
    QSettings& s = store();
    s.setValue(itemKey(hash),
               QString::fromUtf8(QJsonDocument(AudiobookMeta::toJson(m)).toJson(QJsonDocument::Compact)));
    s.sync();
    gCache.insert(hash, m);
    fireChanged();
}

} // namespace

QString AudiobookMatches::hashKey(const QString& bookKey)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bookKey.toUtf8(), QCryptographicHash::Md5).toHex());
}

Match AudiobookMatches::get(const QString& bookKey)
{
    if (bookKey.isEmpty()) return Match{};
    ensureCache();
    return gCache.value(hashKey(bookKey));
}

bool AudiobookMatches::has(const QString& bookKey)
{
    if (bookKey.isEmpty()) return false;
    ensureCache();
    return gCache.contains(hashKey(bookKey));
}

bool AudiobookMatches::isRejected(const QString& bookKey)
{
    return get(bookKey).rejected;
}

QHash<QString, Match> AudiobookMatches::forBooks(const QStringList& bookKeys)
{
    ensureCache();
    QHash<QString, Match> out;
    if (gCache.isEmpty()) return out;   // the common case: nothing matched yet, and no hashing at all
    for (const QString& k : bookKeys)
    {
        if (k.isEmpty()) continue;
        const auto it = gCache.constFind(hashKey(k));
        if (it != gCache.constEnd()) out.insert(k, it.value());
    }
    return out;
}

void AudiobookMatches::set(const QString& bookKey, const Match& in)
{
    if (bookKey.isEmpty()) return;
    ensureCache();
    const QString h = hashKey(bookKey);
    // A REJECTION IS NOT OVERWRITABLE BY A SWEEP. The bit exists so the user's "no" survives the very code
    // path that would otherwise put the same record back; a caller that wants to match this book again
    // clears the record first, which is a deliberate act with its own name.
    if (gCache.value(h).rejected) return;
    Match m = in;
    m.rejected = false;
    if (m.isEmpty()) return;            // nothing to remember; never record a match that said nothing
    m.updatedAt = QDateTime::currentMSecsSinceEpoch();
    writeRecord(h, m);
}

void AudiobookMatches::reject(const QString& bookKey)
{
    if (bookKey.isEmpty()) return;
    ensureCache();
    const QString h = hashKey(bookKey);
    const Match had = gCache.value(h);
    Match m;
    m.provider   = had.provider;      // what was refused, kept so the surface can still name it
    m.matchId    = had.matchId;
    m.matchTitle = had.matchTitle;
    m.matchAuthor = had.matchAuthor;
    m.rejected   = true;
    m.updatedAt  = QDateTime::currentMSecsSinceEpoch();
    writeRecord(h, m);
}

void AudiobookMatches::clear(const QString& bookKey)
{
    if (bookKey.isEmpty()) return;
    ensureCache();
    const QString h = hashKey(bookKey);
    if (!gCache.contains(h)) return;
    QSettings& s = store();
    s.remove(itemKey(h));
    s.sync();
    gCache.remove(h);
    fireChanged();
}

void AudiobookMatches::clearAll()
{
    ensureCache();
    if (gCache.isEmpty()) return;
    QSettings& s = store();
    s.remove(kItemsGroup);
    s.sync();
    gCache.clear();
    fireChanged();
}

int AudiobookMatches::count()
{
    ensureCache();
    int n = 0;
    for (auto it = gCache.constBegin(); it != gCache.constEnd(); ++it)
        if (!it.value().rejected && it.value().hasFields()) ++n;
    return n;
}

int AudiobookMatches::rejectedCount()
{
    ensureCache();
    int n = 0;
    for (auto it = gCache.constBegin(); it != gCache.constEnd(); ++it)
        if (it.value().rejected) ++n;
    return n;
}

void AudiobookMatches::invalidate()
{
    gCacheBuilt = false;
    gCache.clear();
}

void AudiobookMatches::setChangeHook(std::function<void()> hook)
{
    g_changeHook = std::move(hook);
}
