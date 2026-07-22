#include "ItemMarks.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QSettings>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>

// Shares the portable mymediavault.ini with the other stores (same AppPaths::dataDir() posture). Coherence
// with any other QSettings on the same file comes from every writer calling sync() (flush to disk); QSettings
// reloads on access when the on-disk file changed.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

namespace {

using ItemMarks::Completion;
using ItemMarks::Marks;

// Per-profile group root: "marks/<profileId>" (or "marks/default" when no profile is selected). Same
// per-profile mechanic as FavoritesStore/PlaylistStore.
QString profileGroup()
{
    const QString id = ProfileStore::currentId();
    return QStringLiteral("marks/") + (id.isEmpty() ? QStringLiteral("default") : id);
}

// MD5-hex token of the opaque caller key — flattens paths/URLs (which may carry '/', '//', or trailing
// separators that QSettings would normalize into colliding group paths) to a collision-safe leaf. Same hash
// pattern as SyncOffsets::fileKey. Only ever built for a non-empty key (callers guard empty first).
QString hashKey(const QString& key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

QString itemsGroup() { return profileGroup() + QStringLiteral("/items"); }
QString itemKey(const QString& hash) { return itemsGroup() + QLatin1Char('/') + hash; }
QString vocabKey()   { return profileGroup() + QStringLiteral("/tagVocab"); }
QString pinnedKey()  { return profileGroup() + QStringLiteral("/pinnedTags"); }

// Completion <-> stable string tokens (forward-compatible; an unknown/absent token reads back as None).
QString completionToString(Completion c)
{
    switch (c)
    {
        case Completion::InProgress: return QStringLiteral("inProgress");
        case Completion::Finished:   return QStringLiteral("finished");
        case Completion::Abandoned:  return QStringLiteral("abandoned");
        case Completion::Planned:    return QStringLiteral("planned");
        case Completion::None:       break;
    }
    return QStringLiteral("none");
}

Completion completionFromString(const QString& s)
{
    if (s == QStringLiteral("inProgress")) return Completion::InProgress;
    if (s == QStringLiteral("finished"))   return Completion::Finished;
    if (s == QStringLiteral("abandoned"))  return Completion::Abandoned;
    if (s == QStringLiteral("planned"))    return Completion::Planned;
    return Completion::None; // "none", unknown, or corrupt
}

bool isDefault(const Marks& m)
{
    return !m.hidden && m.completion == Completion::None && m.tags.isEmpty();
}

Marks marksFromJson(const QByteArray& json)
{
    Marks m;
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    m.hidden     = o.value(QStringLiteral("hidden")).toBool();
    m.completion = completionFromString(o.value(QStringLiteral("completion")).toString());
    for (const QJsonValue& v : o.value(QStringLiteral("tags")).toArray())
    {
        const QString t = v.toString();
        if (!t.isEmpty()) m.tags.push_back(t);
    }
    return m;
}

QByteArray marksToJson(const Marks& m)
{
    QJsonObject o;
    o.insert(QStringLiteral("hidden"), m.hidden);
    o.insert(QStringLiteral("completion"), completionToString(m.completion));
    QJsonArray arr;
    for (const QString& t : m.tags) arr.append(t);
    o.insert(QStringLiteral("tags"), arr);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QStringList readStringArray(const QString& key)
{
    QStringList out;
    const QByteArray json = store().value(key).toString().toUtf8();
    for (const QJsonValue& v : QJsonDocument::fromJson(json).array())
    {
        const QString s = v.toString();
        if (!s.isEmpty() && !out.contains(s)) out.push_back(s);
    }
    return out;
}

void writeStringArray(const QString& key, const QStringList& list)
{
    if (list.isEmpty()) { store().remove(key); store().sync(); return; }
    QJsonArray arr;
    for (const QString& s : list) arr.append(s);
    store().setValue(key, QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
}

// ---- Lazy per-profile cache -------------------------------------------------------------------------------
// get() is called once per catalog item during hidden-filtering/shelf-building, so parse the whole profile's
// items ONCE into a QHash<itemHash, Marks> and serve lookups from it. mCacheGroup records which profile the
// cache is for so a profile switch rebuilds transparently; invalidate() forces a rebuild for external ini
// changes. mAnyHidden is precomputed during the build so anyHidden() is O(1).
QString mCacheGroup;          // the itemsGroup() the cache was built for; empty => not built
bool    mCacheBuilt = false;
QHash<QString, Marks> mCache; // itemHash -> Marks
bool    mAnyHidden = false;

void ensureCache()
{
    const QString grp = itemsGroup();
    if (mCacheBuilt && mCacheGroup == grp) return;

    mCache.clear();
    mAnyHidden = false;

    QSettings& s = store();
    s.beginGroup(grp);
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
    {
        const Marks m = marksFromJson(s.value(h).toString().toUtf8());
        if (isDefault(m)) continue; // ignore any stale all-default blob
        mCache.insert(h, m);
        if (m.hidden) mAnyHidden = true;
    }
    s.endGroup();

    mCacheGroup = grp;
    mCacheBuilt = true;
}

// Load one item's marks directly from the store (writers read-modify-write independent of the cache).
Marks loadItem(const QString& hash)
{
    return marksFromJson(store().value(itemKey(hash)).toString().toUtf8());
}

// Persist one item's marks: an all-default blob is removed (never leave junk), else written. Invalidates.
void saveItem(const QString& hash, const Marks& m)
{
    if (isDefault(m)) store().remove(itemKey(hash));
    else              store().setValue(itemKey(hash), QString::fromUtf8(marksToJson(m)));
    store().sync();
    ItemMarks::invalidate();
}

} // namespace

Marks ItemMarks::get(const QString& key)
{
    if (key.isEmpty()) return Marks{};
    ensureCache();
    return mCache.value(hashKey(key)); // absent -> default Marks{}
}

void ItemMarks::setHidden(const QString& key, bool hidden)
{
    if (key.isEmpty()) return;
    const QString h = hashKey(key);
    Marks m = loadItem(h);
    m.hidden = hidden;
    saveItem(h, m);
}

void ItemMarks::setCompletion(const QString& key, Completion c)
{
    if (key.isEmpty()) return;
    const QString h = hashKey(key);
    Marks m = loadItem(h);
    m.completion = c;
    saveItem(h, m);
}

void ItemMarks::setTags(const QString& key, const QStringList& tags)
{
    if (key.isEmpty()) return;

    // Normalize: drop empties/dupes, preserve order.
    QStringList clean;
    for (const QString& t : tags)
        if (!t.isEmpty() && !clean.contains(t)) clean.push_back(t);

    const QString h = hashKey(key);
    Marks m = loadItem(h);
    m.tags = clean;
    saveItem(h, m);

    // Union any new tags into the profile's vocabulary.
    QStringList vocab = readStringArray(vocabKey());
    bool grew = false;
    for (const QString& t : clean)
        if (!vocab.contains(t)) { vocab.push_back(t); grew = true; }
    if (grew) writeStringArray(vocabKey(), vocab);
}

QStringList ItemMarks::tagVocab()
{
    return readStringArray(vocabKey());
}

void ItemMarks::removeTagEverywhere(const QString& tag)
{
    if (tag.isEmpty()) return;

    // 1. Drop from the vocabulary.
    QStringList vocab = readStringArray(vocabKey());
    if (vocab.removeAll(tag) > 0) writeStringArray(vocabKey(), vocab);

    // 2. Unpin it.
    QStringList pinned = readStringArray(pinnedKey());
    if (pinned.removeAll(tag) > 0) writeStringArray(pinnedKey(), pinned);

    // 3. Strip it from every item in this profile (rewrite only the ones that carried it; a blob left
    //    all-default after the strip is removed).
    QSettings& s = store();
    const QString grp = itemsGroup();
    s.beginGroup(grp);
    const QStringList hashes = s.childKeys();
    s.endGroup();
    for (const QString& h : hashes)
    {
        Marks m = loadItem(h);
        if (m.tags.removeAll(tag) > 0)
        {
            if (isDefault(m)) s.remove(itemKey(h));
            else              s.setValue(itemKey(h), QString::fromUtf8(marksToJson(m)));
        }
    }
    s.sync();
    invalidate();
}

QStringList ItemMarks::pinnedTags()
{
    return readStringArray(pinnedKey());
}

void ItemMarks::setPinned(const QString& tag, bool pinned)
{
    if (tag.isEmpty()) return;
    QStringList list = readStringArray(pinnedKey());
    const bool has = list.contains(tag);
    if (pinned && !has)  list.push_back(tag);
    else if (!pinned && has) list.removeAll(tag);
    else return; // no change
    writeStringArray(pinnedKey(), list);
}

QVector<QString> ItemMarks::itemKeysWithTag(const QString& tag)
{
    QVector<QString> out;
    if (tag.isEmpty()) return out;
    ensureCache();
    for (auto it = mCache.constBegin(); it != mCache.constEnd(); ++it)
        if (it.value().tags.contains(tag)) out.push_back(it.key());
    return out;
}

bool ItemMarks::anyHidden()
{
    ensureCache();
    return mAnyHidden;
}

void ItemMarks::invalidate()
{
    mCacheBuilt = false;
    mCacheGroup.clear();
    mCache.clear();
    mAnyHidden = false;
}
