#include "MusicRemap.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "MusicId.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSet>
#include <QStringList>
#include <QVariant>
#include <algorithm>

// Like PcGameRemap, this unit works at the QSettings layer rather than through the store classes, and for
// the same three reasons that file gives at length: EXACTNESS (the stores' writers are accrual APIs — there
// is no "set the absolute total" entry point on ConsumptionStats at all — so replaying a record through them
// would rewrite the very fields the move exists to preserve), COVERAGE (the stores read the ACTIVE profile
// and write only THIS device's namespace, while a record belongs to whichever profile accrued it), and
// LEANNESS (staying on QtCore is what lets probe_musicremap link against Qt6::Core alone).
//
// The Batch class and the two hashes below are deliberate near-copies of PcGameRemap.cpp's. They are NOT
// shared through a common header, and that is a decision rather than an oversight: extracting the internals
// of a data migration that has already shipped and already migrated real installs is a refactor with its own
// risk, and it does not belong inside an unrelated increment. Each helper names the source line it mirrors,
// and probe_musicremap recomputes every key shape independently rather than calling these helpers, so a
// drift between the two shows up as a failing check instead of as a passing tautology. A shared `ItemRemap`
// seam is the obvious follow-up; it should move both units at once, under both probes.

namespace {

#ifdef EB_MUSICID_TEST_SEAM
QString    g_testIniPath;
QSettings* g_testStore = nullptr;
#endif

std::function<void()> g_invalidate;

void syncStore(QSettings& s) { s.sync(); }

QSettings& store()
{
#ifdef EB_MUSICID_TEST_SEAM
    if (!g_testIniPath.isEmpty())
    {
        // Deleting the cached QSettings rather than only re-pointing the path is the load-bearing half: a
        // function-local static is constructed exactly once, so a path captured on first use would pin every
        // later case to the first one's file. MusicId's seam has the same shape for the same reason.
        if (!g_testStore) g_testStore = new QSettings(g_testIniPath, QSettings::IniFormat);
        return *g_testStore;
    }
#endif
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// ---- the two key hashes, each mirroring one store exactly ------------------------------------------------
// ConsumptionStats::hashKey (ConsumptionStats.cpp:96) and SyncOffsets' fileKey token (SyncOffsets.cpp:39):
// FULL md5 hex.
QString md5Hex(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

// ResumeStore::hashFor (ResumeStore.cpp:13) and SpeedStore::hashFor (SpeedStore.cpp:23): md5 hex TRUNCATED
// to the first 10 characters. A second, easily-missed shape over the same digest.
QString md5Hex10(const QString& key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex().left(10));
}

// ---- write-then-verify, ONE FLUSH PER PASS ---------------------------------------------------------------
// PcGameRemap.cpp's Batch, and its reasoning applies unchanged: setValue only accumulates into QSettings'
// in-memory map, so it costs nothing per record; commit() flushes ONCE and only then retires the sources, so
// a durability failure drops every queued removal and leaves every source standing (rule 2 at pass
// granularity, which is strictly more conservative than per record). The per-record read-back is a
// SERIALISATION gate, not a durability one — it catches a value whose ini round-trip is not identity, which
// would read back wrong in the next process, i.e. after the source had already been retired.
//
// AND IT IS A TRIPWIRE THAT NO TEST CAN TRIP, WHICH IS WHY IT IS SAID OUT LOUD HERE. value() answers out of
// the same in-memory map setValue just wrote, so within one process the comparison is tautological: `ok` is
// always true and every `if (!ok) continue;` below is unreachable. It stays because it costs nothing and
// because the day a QVariant type is introduced whose ini spelling is not its own toString(), this is the
// line that refuses to retire the source. Do not delete it as dead code, and do not expect a mutant of it to
// be killed — the reachable half of rule 2 (retire the source without writing the destination at all) is
// mutated instead, in musicremap-mutants.json.
class Batch
{
public:
    explicit Batch(QSettings& s) : s_(s) {}

    bool write(const QString& key, const QVariant& value)
    {
        s_.setValue(key, value);
        dirty_ = true;
        return s_.value(key).toString() == value.toString();
    }

    void clearNow(const QString& key) { s_.remove(key); dirty_ = true; }
    void removeLater(const QString& key) { pending_.push_back(key); }

    bool commit()
    {
        if (!dirty_ && pending_.isEmpty()) return true;   // nothing to do: do not rewrite the file
        syncStore(s_);
        dirty_ = false;
        if (s_.status() != QSettings::NoError) { pending_.clear(); return false; }
        if (pending_.isEmpty()) return true;
        for (const QString& k : pending_) s_.remove(k);   // a group key removes its subkeys too
        pending_.clear();
        syncStore(s_);
        return s_.status() == QSettings::NoError;
    }

    bool touched() const { return moved_; }
    void noteMoved() { moved_ = true; }

private:
    QSettings&  s_;
    QStringList pending_;
    bool        dirty_ = false;
    bool        moved_ = false;
};

// ---- ConsumptionStats merge -------------------------------------------------------------------------------
// mediaSeconds: SUM. It is a pure accumulator and the per-category rollup (cat/audio/seconds) is maintained
//   as the sum of every item's seconds, so summing is the only rule that leaves the rollup coherent with the
//   items it is a rollup of. Moving a row WITHIN one profile+device root, or folding two of them together,
//   therefore leaves that rollup exactly as it was — which is why this pass never touches it.
// pagesRead: MAX, not sum — its writer defines it as the high-water page ever reached, so adding two would
//   invent pages nobody read. (Always zero for a music track; the rule is stated so nobody "fixes" it.)
// lastActivity: MAX, and the side that owns it owns title/category too — ConsumptionStats::ensureCache
//   already folds two devices' copies of one item that way.
QString mergeStats(const QString& dstJson, const QString& srcJson)
{
    const QJsonObject a = QJsonDocument::fromJson(dstJson.toUtf8()).object();
    const QJsonObject b = QJsonDocument::fromJson(srcJson.toUtf8()).object();

    const qint64 la = qint64(a.value(QStringLiteral("lastActivity")).toDouble());
    const qint64 lb = qint64(b.value(QStringLiteral("lastActivity")).toDouble());
    const QJsonObject& newer = (lb > la) ? b : a;
    const QJsonObject& older = (lb > la) ? a : b;

    QString title = newer.value(QStringLiteral("title")).toString();
    if (title.isEmpty()) title = older.value(QStringLiteral("title")).toString();
    QString cat = newer.value(QStringLiteral("category")).toString();
    if (cat.isEmpty()) cat = older.value(QStringLiteral("category")).toString();

    QJsonObject o;
    o.insert(QStringLiteral("mediaSeconds"),
             double(qint64(a.value(QStringLiteral("mediaSeconds")).toDouble())
                  + qint64(b.value(QStringLiteral("mediaSeconds")).toDouble())));
    o.insert(QStringLiteral("pagesRead"),
             double(std::max(qint64(a.value(QStringLiteral("pagesRead")).toDouble()),
                             qint64(b.value(QStringLiteral("pagesRead")).toDouble()))));
    o.insert(QStringLiteral("lastActivity"), double(std::max(la, lb)));
    o.insert(QStringLiteral("title"), title);
    o.insert(QStringLiteral("category"), cat);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// ---- per-store passes --------------------------------------------------------------------------------------
// Each pass is written the same way: locate the source record, skip if absent, merge into the destination,
// stage the write, queue the source for removal; the pass commits ONCE. A failed write, or a commit that
// reports an error, leaves BOTH copies in place and the next rebuild retries. Nothing here can reach a state
// where neither copy exists.

// resume/<md5(playId) truncated to 10>/{pos,dur,ts,title} — global, not per-profile.
//
// NEWEST WINS rather than an accumulation, and that is not a shortcut: a resume position is a single point
// in a single stream, so there is nothing additive about two of them and a sum or a max of `pos` would
// produce a position that was never reached in either copy. The newer `ts` is where the user last left off.
//
// RETIRING THE SOURCE RECORDS NO TOMBSTONE, deliberately. #150's dated husk says "the user forgot this"; a
// re-key says "the same position now lives under a different name". Dating it would tell every peer to
// forget a position that was not forgotten — including a peer that has not run this repair and is still the
// only device holding it. A device-local repair is not an edit. The cost is one orphan row under a key
// nothing looks up any more, which goes away when that peer merges its own copies too.
void remapResume(QSettings& s, const QHash<QString, QString>& table, Batch& b)
{
    for (auto it = table.cbegin(); it != table.cend(); ++it)
    {
        if (it.key() == it.value()) continue;
        const QString src = QStringLiteral("resume/") + md5Hex10(it.key());
        const QString dst = QStringLiteral("resume/") + md5Hex10(it.value());
        if (src == dst) continue;

        const bool has = s.contains(src + QStringLiteral("/pos")) || s.contains(src + QStringLiteral("/dur"));
        if (!has) continue;

        const bool dstHas = s.contains(dst + QStringLiteral("/pos"))
                         || s.contains(dst + QStringLiteral("/dur"));
        const qint64 srcTs = s.value(src + QStringLiteral("/ts"), 0).toLongLong();
        const qint64 dstTs = s.value(dst + QStringLiteral("/ts"), 0).toLongLong();

        if (!dstHas || srcTs > dstTs)
        {
            // Wholesale: a resume record's leaves only mean anything together, so a leaf the winner does not
            // carry is CLEARED rather than left standing (a newer `pos` of 30 beside an inherited `dur` of
            // 100 renders a progress bar for a position that was never 30% of anything).
            bool ok = true;
            for (const char* leaf : { "/pos", "/dur", "/title" })
            {
                const QString lk = src + QLatin1String(leaf);
                if (s.contains(lk)) ok = b.write(dst + QLatin1String(leaf), s.value(lk)) && ok;
            }
            if (!ok) continue;                          // leave BOTH in place; the next rebuild retries
            for (const char* leaf : { "/pos", "/dur", "/title" })
                if (!s.contains(src + QLatin1String(leaf))) b.clearNow(dst + QLatin1String(leaf));

            // `ts` LAST, because it is the field the comparison above reads: written first, a run that died
            // halfway would leave the destination LOOKING newer than the source and the retry would skip it
            // and retire the source on top of a half-copied record.
            const QString sts = src + QStringLiteral("/ts");
            if (s.contains(sts)) { if (!b.write(dst + QStringLiteral("/ts"), s.value(sts))) continue; }
            else                 b.clearNow(dst + QStringLiteral("/ts"));
        }
        b.removeLater(src);
        b.noteMoved();
    }
}

// stats/<profile>/<device>/items/<md5(playId)>, plus the pre-namespacing shape stats/<profile>/items/<md5>
// that ConsumptionStats::migrateStatsProfile folds away — both handled, because this may well run before
// that fold on a machine upgrading from an old build and a record it failed to see would be orphaned by the
// fold a moment later.
//
// THIS PASS NEVER WRITES `lastWrite`. That leaf is stamped only by a namespace's OWNER at a real accrual and
// it is the entire freshness gate CloudMerge::mergeNamespaced uses. A repair that stamped it — including in
// the foreign namespaces this pass rewrites, which this device does not own — would outrank the owner's
// genuine data and hand every peer this device's rekeyed copy in place of the one it is still reading.
void remapConsumption(QSettings& s, const QHash<QString, QString>& table, Batch& b)
{
    s.beginGroup(QStringLiteral("stats"));
    const QStringList profiles = s.childGroups();
    s.endGroup();

    for (const QString& p : profiles)
    {
        const QString base = QStringLiteral("stats/") + p;
        s.beginGroup(base);
        const QStringList children = s.childGroups();
        s.endGroup();

        QStringList itemRoots;
        itemRoots << base + QStringLiteral("/items/");                  // legacy, un-namespaced
        for (const QString& c : children)
        {
            if (c == QLatin1String("items") || c == QLatin1String("cat")) continue;   // legacy roots, above
            itemRoots << base + QLatin1Char('/') + c + QStringLiteral("/items/");     // a device namespace
        }

        for (const QString& root : itemRoots)
            for (auto it = table.cbegin(); it != table.cend(); ++it)
            {
                if (it.key() == it.value()) continue;
                const QString src = root + md5Hex(it.key());
                const QString dst = root + md5Hex(it.value());
                if (src == dst) continue;
                const QString srcBlob = s.value(src).toString();
                if (srcBlob.isEmpty()) continue;
                const QString dstBlob = s.value(dst).toString();
                // Nothing at the destination -> move the blob VERBATIM (which keeps lastActivity, the field
                // the multi-device merge orders on). Something there -> merge, never overwrite.
                const QString merged = dstBlob.isEmpty() ? srcBlob : mergeStats(dstBlob, srcBlob);
                if (!b.write(dst, merged)) continue;
                b.removeLater(src);
                b.noteMoved();
            }
    }
}

// speed/items/<md5(indexId) truncated to 10> -> one JSON blob holding the playback rate.
//
// THE DESTINATION WINS when it already has one, and there is nothing to merge: a rate is a preference the
// user set while listening to that record, and two of them are two statements about the same thing with no
// arithmetic between them. Keeping the destination's is the conservative half — it is the copy the app is
// about to play, so it is the answer the user most recently expressed for the row now on screen.
void remapSpeed(QSettings& s, const QHash<QString, QString>& table, Batch& b)
{
    for (auto it = table.cbegin(); it != table.cend(); ++it)
    {
        if (it.key() == it.value()) continue;
        const QString src = QStringLiteral("speed/items/") + md5Hex10(it.key());
        const QString dst = QStringLiteral("speed/items/") + md5Hex10(it.value());
        if (src == dst) continue;
        const QString blob = s.value(src).toString();
        if (blob.isEmpty()) continue;
        if (s.value(dst).toString().isEmpty() && !b.write(dst, blob)) continue;
        b.removeLater(src);
        b.noteMoved();
    }
}

// sync/files/<md5(indexId)>/{audio,sub} -> the per-file A/V and subtitle sync offsets.
//
// Same rule as speed and for the same reason: an offset is a setting, not an accumulation, and the
// destination's own is never overwritten. Both axes move together or not at all — they are two halves of one
// per-file adjustment, and half of one is worse than neither.
void remapSyncOffsets(QSettings& s, const QHash<QString, QString>& table, Batch& b)
{
    for (auto it = table.cbegin(); it != table.cend(); ++it)
    {
        if (it.key() == it.value()) continue;
        const QString src = QStringLiteral("sync/files/") + md5Hex(it.key());
        const QString dst = QStringLiteral("sync/files/") + md5Hex(it.value());
        if (src == dst) continue;

        bool had = false, ok = true;
        for (const char* leaf : { "/audio", "/sub" })
        {
            const QString lk = src + QLatin1String(leaf);
            if (!s.contains(lk)) continue;
            had = true;
            const QString dk = dst + QLatin1String(leaf);
            if (!s.contains(dk)) ok = b.write(dk, s.value(lk)) && ok;
        }
        if (!had || !ok) continue;
        b.removeLater(src);
        b.noteMoved();
    }
}

// ---- the pure matcher ------------------------------------------------------------------------------------
// Reused rather than reinvented: MusicId::normalizeAlbum is this app's ONE title normaliser (case,
// diacritics, `&`, edition and remaster noise, punctuation, sequel words, Roman numerals). It is the right
// one for a track title too — "Airbag (2009 Remaster)" and "Airbag" are the same track — and it is
// conservative in the direction that matters: a title that normalises to nothing matches nothing.
QString titleKey(const QString& raw) { return MusicId::normalizeAlbum(raw); }

// Add old -> neu, unless it would be empty, a self-map, or a SECOND, DIFFERENT destination for an identity
// something else already claimed. The last case cannot arise from a well-formed merge (an instance belongs to
// one group), but it is exactly the shape that would send a record to a key nothing reads, so it resolves the
// only safe way: the identity is banned from the table entirely and its records stay where they are.
void offer(QHash<QString, QString>& out, QSet<QString>& banned, const QString& old, const QString& neu)
{
    if (old.isEmpty() || neu.isEmpty() || old == neu) return;
    if (banned.contains(old)) return;
    const auto had = out.constFind(old);
    if (had != out.constEnd())
    {
        if (*had == neu) return;
        out.remove(old);
        banned.insert(old);
        return;
    }
    out.insert(old, neu);
}

} // namespace

// EVERY TRACK ONTO ITSELF, from the name the player used to be handed to the name it has always answered to
// in the index. The whole of #204's migration; the rules it needs are `offer`'s and the header says why each
// one is load-bearing here rather than merely tidy.
MusicRemap::Table MusicRemap::streamKeyTable(const QVector<TrackId>& tracks)
{
    Table t;
    QSet<QString> banned;
    for (const TrackId& tr : tracks)
        offer(t.map, banned, tr.playId, tr.indexId);
    return t;
}

MusicRemap::Table MusicRemap::tableFor(const QVector<AlbumGroup>& groups)
{
    Table t;
    QSet<QString> banned;

    for (const AlbumGroup& g : groups)
    {
        if (g.instances.size() < 2) continue;                 // not merged: nothing has moved
        const Instance& primary = g.instances.first();
        if (primary.tracks.isEmpty()) continue;               // no destination yet — rule 1, wait

        // Which numbers and titles are usable on the PRIMARY side: unique ones only. A duplicate track
        // number (two discs numbered from 1, a sloppy tag) or two tracks with the same normalised title make
        // that key ambiguous, and an ambiguous match is a wrong record moved silently.
        QHash<int, int>     numCount;
        QHash<QString, int> titleCount;
        for (const TrackId& tr : primary.tracks)
        {
            if (tr.number > 0) numCount[tr.number]++;
            const QString k = titleKey(tr.title);
            if (!k.isEmpty()) titleCount[k]++;
        }

        for (int i = 1; i < g.instances.size(); ++i)
        {
            const Instance& other = g.instances.at(i);
            if (other.tracks.isEmpty()) continue;             // this copy has not been fetched — rule 1

            QHash<int, int>     otherNum;
            QHash<QString, int> otherTitle;
            for (const TrackId& tr : other.tracks)
            {
                if (tr.number > 0) otherNum[tr.number]++;
                const QString k = titleKey(tr.title);
                if (!k.isEmpty()) otherTitle[k]++;
            }

            for (const TrackId& tr : other.tracks)
            {
                const TrackId* to = nullptr;
                // Pass 1: the track number, when it is unique on BOTH sides. A number is what a rip and a
                // server agree on even when their titles are spelled differently.
                if (tr.number > 0 && numCount.value(tr.number) == 1 && otherNum.value(tr.number) == 1)
                {
                    for (const TrackId& p : primary.tracks)
                        if (p.number == tr.number) { to = &p; break; }
                }
                // Pass 2: the normalised title, same uniqueness rule. This is what carries an untagged rip
                // whose numbers are all zero.
                if (!to)
                {
                    const QString k = titleKey(tr.title);
                    if (!k.isEmpty() && titleCount.value(k) == 1 && otherTitle.value(k) == 1)
                        for (const TrackId& p : primary.tracks)
                            if (titleKey(p.title) == k) { to = &p; break; }
                }
                if (!to) continue;                            // unmatched: its records stay put — rule 1

                // THE INDEX IDENTITY AND NOTHING ELSE (#204). This used to offer the play identities into a
                // second table as well, because the resume and consumption stores keyed on those. They key on
                // this one now, for every route, so a second table would be a second answer to a question
                // that has one.
                offer(t.map, banned, tr.indexId, to->indexId);
            }
        }
    }
    return t;
}

void MusicRemap::applyRemap(const Table& table)
{
    // The single-source guarantee, and the cheap path for everything else: with nothing to move we do not
    // open the store, do not read a key and do not write one. An install with one music source produces an
    // empty table by construction (MusicMerge short-circuits, so there are no groups at all).
    if (table.isEmpty()) return;

    QSettings& s = store();
    Batch b(s);
    // ALL FOUR PASSES OFF ONE MAP (#204). Two of them (resume, consumption) used to be driven by a separate
    // playback-identity table; they key on the durable identity now, so every store this unit sweeps is
    // keyed the same way and takes the same table.
    //
    // The #204 MIGRATION table goes through the identical four passes, which is deliberate and is more than
    // symmetry: the speed and sync stores key on syncKey_, and syncKey_ was itself a raw signed url before
    // #193 taught it the index path — so a long-lived install can hold speed/sync rows under a stream url
    // too, and sweeping them costs nothing and repairs those as well.
    remapResume(s, table.map, b);
    remapConsumption(s, table.map, b);
    remapSpeed(s, table.map, b);
    remapSyncOffsets(s, table.map, b);
    b.commit();
    if (b.touched() && g_invalidate) g_invalidate();
}

void MusicRemap::setCacheInvalidator(std::function<void()> fn) { g_invalidate = std::move(fn); }

#ifdef EB_MUSICID_TEST_SEAM
void MusicRemap::setRemapIniPathForTesting(const QString& path)
{
    g_testIniPath = path;
    delete g_testStore;
    g_testStore = nullptr;
}
#endif
