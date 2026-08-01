#include "ItemMarks.h"
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
#include <QHash>

// Shares the portable everythingbox.ini with the other stores (same AppPaths::dataDir() posture). Coherence
// with any other QSettings on the same file comes from every writer calling sync() (flush to disk); QSettings
// reloads on access when the on-disk file changed.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

namespace {

using ItemMarks::Completion;
using ItemMarks::Marks;

// Change-callback (mdsync T2): fired after every mutation to (re)arm the debounced Drive push. Set once by
// MainWindow; null (a no-op) in headless probes.
std::function<void()> g_changeHook;
void fireChanged() { if (g_changeHook) g_changeHook(); }

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

// No mark set. Ignores updatedAt, so a CLEARED item's husk (an all-default blob carrying a fresh stamp) is
// "default" for every reader while still being a real, newer, propagating record. See saveItem.
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
    m.updatedAt  = static_cast<qint64>(o.value(QStringLiteral("updatedAt")).toDouble());
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
    o.insert(QStringLiteral("updatedAt"), static_cast<double>(m.updatedAt));
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
    if (list.isEmpty()) { store().remove(key); store().sync(); fireChanged(); return; }
    QJsonArray arr;
    for (const QString& s : list) arr.append(s);
    store().setValue(key, QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    store().sync();
    fireChanged();
}

// ---- Lazy per-profile cache -------------------------------------------------------------------------------
// get() is the hot path — hidden-filtering/shelf-building calls it once per catalog item. Its true per-call
// cost is a ProfileStore::currentId() read + a QString compare (the self-healing profile-switch check) + one
// MD5 of the caller key for the QHash lookup. The expensive parts — resolving the "marks/<id>/items" group
// string and parsing every stored blob — happen ONCE at (re)build time and are reused: mCacheItemsGroup is the
// resolved group, mCacheProfileId records which profile the cache is for so a profile switch rebuilds
// transparently, and mAnyHidden is precomputed so anyHidden() is O(1). invalidate() forces a rebuild for
// external ini changes; ItemMarks' own writers invalidate for you.
QString mCacheProfileId;      // profile id the cache was built for; empty => not built (compared cheaply per get())
QString mCacheItemsGroup;     // resolved itemsGroup() for that profile — computed ONCE at build, not per get()
bool    mCacheBuilt = false;
QHash<QString, Marks> mCache; // itemHash -> Marks
bool    mAnyHidden = false;

void ensureCache()
{
    // Cheap self-healing check: a single currentId() read + QString compare (NOT a full itemsGroup() rebuild
    // with its "marks/" + "/items" concatenations) decides whether the cached profile is still current.
    const QString id = ProfileStore::currentId();
    if (mCacheBuilt && mCacheProfileId == id) return;

    mCache.clear();
    mAnyHidden = false;
    mCacheProfileId = id;
    // Resolve the group ONCE from the id we just read (mirrors profileGroup()/itemsGroup(); reusing `id` avoids
    // a second currentId() read and guarantees the built group matches the id the cache is keyed on).
    mCacheItemsGroup = QStringLiteral("marks/") + (id.isEmpty() ? QStringLiteral("default") : id)
                       + QStringLiteral("/items");

    QSettings& s = store();
    s.beginGroup(mCacheItemsGroup);
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
    {
        const Marks m = marksFromJson(s.value(h).toString().toUtf8());
        if (isDefault(m)) continue; // ignore any stale all-default blob
        mCache.insert(h, m);
        if (m.hidden) mAnyHidden = true;
    }
    s.endGroup();

    mCacheBuilt = true;
}

// Load one item's marks directly from the store (writers read-modify-write independent of the cache).
Marks loadItem(const QString& hash)
{
    return marksFromJson(store().value(itemKey(hash)).toString().toUtf8());
}

// Persist one item's marks, with a fresh updatedAt stamp (the merge funnel — every content write bumps the
// item's timestamp).
//
// A row cleared back to all-default is NOT removed. THE RULE, here and in every store that merges by
// timestamp: "cleared" and "never known" must not have the same representation. A deleted row is
// indistinguishable from "this device has never seen that item", so the next merge with a peer still holding
// the old marks reads present-on-them / absent-on-us — ignorance, not a newer clear — and CloudMerge's
// never-delete marks pass puts back exactly what the user just cleared, on both devices (issue #132). The
// cleared row is therefore left as a stamped HUSK: an all-default blob carrying a fresh updatedAt, which is a
// fact with a time on it that mergeMarks can compare, and which wins. Same idiom, same reason, as
// MetaOverrides::reset() (issue #24) — one spelling of this, not two.
//
// Nothing on the READ side had to change, and that is a property of the shape rather than luck: a husk IS an
// all-default blob, so get() / anyHidden() / itemKeysWithTag() answer "no marks" for it by construction, on
// this build and on every build already shipped. A device still running an older binary therefore reads a
// husk correctly even though it cannot write one, which is what lets a mixed-version fleet converge.
// (ensureCache's `if (isDefault(m)) continue` is a size optimisation on top of that, not the mechanism —
// leaving a husk IN the cache would answer every reader identically. It is called out here because no
// assertion can distinguish the two, so nothing else would say so.)
//
// A husk is only ever left where there was a record to CLEAR. Clearing an item that never carried a mark
// (unhiding something never hidden, saving an empty tag list on an untagged item) writes nothing at all:
// nothing was cleared, so "never known" is the truth, and a husk there would record an event that did not
// happen — and would grow the store on every no-op. Re-clearing an item that is ALREADY a husk does re-stamp
// it, which is harmless (the record still says "cleared", now more recently) and keeps the funnel one branch.
//
// Husks are never compacted, deliberately, and for MetaOverrides' reason: a husk has to outlive any peer's
// stale copy of the marks it cleared, and "any peer" has no expiry — a device that has been off for a year
// still holds that copy. So the store keeps one ~85-byte row per (profile, item) ever marked and then
// cleared, and never shrinks. The cost is bounded by deliberate user actions (hide/complete/tag, then undo),
// not by playback, and what it buys off is the user's clear silently undoing itself at the next sync. A
// Tombstones entry would be the bounded alternative and is the WRONG one here: Tombstones::compact(30) runs
// at every merge, so a device dormant for 31 days would resurrect the mark — which is this bug again.
void saveItem(const QString& hash, Marks m)
{
    const QString k = itemKey(hash);
    if (isDefault(m) && !store().contains(k)) return; // nothing stored, so nothing was cleared: stay absent
    m.updatedAt = QDateTime::currentSecsSinceEpoch();
    store().setValue(k, QString::fromUtf8(marksToJson(m)));
    store().sync();
    ItemMarks::invalidate();
    fireChanged();
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
        if (!vocab.contains(t))
        {
            vocab.push_back(t);
            grew = true;
            // Re-creating a previously-retired tag clears its vocab tombstone, so the merge's
            // union-minus-tombstoned pass doesn't strip this genuine re-add on the next pull.
            Tombstones::remove(profileGroup() + QStringLiteral("/tagVocab"), t);
        }
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

    // 3. Strip it from every item in this profile (rewrite only the ones that carried it). An item left
    //    all-default by the strip is REWRITTEN as a stamped husk, not removed — deleting it would hand the
    //    next merge an absence to read as ignorance and let a peer's stale copy restore the tag on an item
    //    whose tag this device just retired (see saveItem). Every hash here came from childKeys(), so a
    //    record exists by construction and the husk always records a real clear.
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
            m.updatedAt = QDateTime::currentSecsSinceEpoch(); // this direct-rewrite path is a write funnel too
            s.setValue(itemKey(h), QString::fromUtf8(marksToJson(m)));
        }
    }
    s.sync();

    // Tag deletion is a removal from the profile's tag VOCABULARY — tombstone the tag NAME in vocab space so
    // the merge doesn't resurrect the retired tag from another device. (Hiding/unhiding an item is NOT a
    // delete and records no tombstone.)
    Tombstones::record(profileGroup() + QStringLiteral("/tagVocab"), tag);

    invalidate();
    fireChanged();
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
    writeStringArray(pinnedKey(), list); // fires the change hook
    // A bare unpin retires the SHELF without deleting the tag: tombstone it in PINNED space (not vocab space —
    // that would delete the tag) so a peer still pinning can't resurrect the shelf on merge (union-minus-
    // tombstoned). Re-pinning clears that tombstone so the genuine re-pin isn't self-suppressed on the next
    // pull. (Tag DELETION — removeTagEverywhere — is what tombstones the vocab space; see there.)
    const QString pinnedSpace = profileGroup() + QStringLiteral("/pinnedTags");
    if (pinned) Tombstones::remove(pinnedSpace, tag);
    else        Tombstones::record(pinnedSpace, tag);
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
    mCacheProfileId.clear();
    mCacheItemsGroup.clear();
    mCache.clear();
    mAnyHidden = false;
}

void ItemMarks::setChangeHook(std::function<void()> hook)
{
    g_changeHook = std::move(hook);
}
