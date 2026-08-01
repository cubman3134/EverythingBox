#include "PcGameRemap.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "PcGameId.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
#include <QVariant>
#include <algorithm>

// This unit works at the QSettings layer rather than through the store classes, and that is deliberate on
// three counts:
//   * EXACTNESS. The stores' writers are accrual APIs, not movers: ItemMarks::setHidden stamps a fresh
//     updatedAt, ConsumptionStats has no "set the absolute total" entry point at all, PlayStats::addSession
//     invents a session. Replaying a record through them would rewrite the very fields the migration exists
//     to preserve. A key-space move preserves the blob byte-for-byte when there is nothing to merge with.
//   * COVERAGE. The stores read the ACTIVE profile (and, for the accumulators, write only THIS device's
//     namespace). The remap has to reach every profile and every device namespace in the file, which means
//     enumerating child groups — the layer below the stores.
//   * LEANNESS. Staying on QtCore keeps probe_pcgames linking against Qt6::Core alone; going through the
//     stores would drag ProfileStore, Settings, FormFactor, Tombstones, RecentStore and DownloadsStore into
//     a probe about identity.
// The cost is that the key shapes and hash functions are duplicated here. Each one is pinned to its source
// line below, and the probe recomputes them independently rather than calling these helpers, so a drift
// between the two shows up as a failing check instead of as a passing tautology.

namespace {

// Test-only redirect (see the header). Deleting the cached QSettings rather than only re-pointing a path is
// the load-bearing half — a function-local static QSettings is constructed exactly once, so a path captured
// on first use would pin every later case to the first one's file. Same shape as PcGameId's seam.
#ifdef EB_PCGAMEID_TEST_SEAM
QString    g_testIniPath;
QSettings* g_testStore = nullptr;
#endif

std::function<void()> g_invalidate;

// Every flush this unit performs goes through here, so the probe can count them (see the header). The
// counter itself exists only under the test seam; the wrapper is unconditional so there is exactly one
// place a sync can be issued from and none can escape the count.
#ifdef EB_PCGAMEID_TEST_SEAM
int g_syncCount = 0;
#endif
void syncStore(QSettings& s)
{
#ifdef EB_PCGAMEID_TEST_SEAM
    ++g_syncCount;
#endif
    s.sync();
}

QSettings& store()
{
#ifdef EB_PCGAMEID_TEST_SEAM
    if (!g_testIniPath.isEmpty())
    {
        if (!g_testStore) g_testStore = new QSettings(g_testIniPath, QSettings::IniFormat);
        return *g_testStore;
    }
#endif
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// ---- the three key hashes, each mirroring one store exactly --------------------------------------------
// ItemMarks::hashKey (ItemMarks.cpp:46) and ConsumptionStats::hashKey (ConsumptionStats.cpp:96): full MD5 hex.
QString md5Hex(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

// PlayStats::sha1Of (PlayStats.cpp:29). NOT md5 — the two accumulators genuinely disagree, and using the
// wrong one here would silently find no record and migrate nothing.
QString sha1Hex(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
}

// The resume key (PlaybackSession.cpp:14, HomeView.cpp:137/158, MainWindow's "resume/<hash>/…"): MD5
// TRUNCATED to the first 10 hex characters. A third distinct shape; the truncation is easy to miss.
QString md5Hex10(const QString& key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex().left(10));
}

// ---- write-then-verify, ONE FLUSH PER PASS ----------------------------------------------------------------
// Rule 2 is unchanged in substance — a record is never removed until its replacement has been written and
// FLUSHED without error — but it is now enforced at PASS granularity instead of per record, because
// QSettings::sync() is a WHOLE-FILE rewrite. Flushing inside every record turned the first-entry migration
// into one disk write per moved record: the remap runs on the GUI thread the first time the PC Games folder
// is opened, so a library with hundreds of migrating records stalled visibly, once, at exactly the moment
// the user is deciding what they think of the folder. Steady state was never the problem; the first pass was.
//
// The batch is the same rule read at the level the flush actually operates on:
//   * setValue accumulates into QSettings' in-memory map. No disk write, so it costs nothing per record.
//   * commit() flushes ONCE and checks status(). Only if that reports no error are the SOURCES removed
//     (a second flush). A durability failure therefore drops every queued removal and leaves every source
//     standing, which is rule 2 for the whole pass rather than for one record.
//   * A source is still never removed before its replacement is on disk. What changed is that "its
//     replacement" is now "every replacement this pass wrote", which is strictly more conservative.
// A torn state — destination written, source removed, only one of them on disk — is not reachable either
// way: both live in one file, so one sync carries both or neither.
//
// WHAT THE READ-BACK DOES AND DOES NOT PROVE. It is NOT a disk read: QSettings answers value() out of the
// in-memory map setValue just wrote, so the comparison is close to tautological and proves nothing about
// what reached the file. It is kept for the one thing it does catch — a value whose ini round-trip is not
// identity (a type QSettings serialises to a different string than it was handed), which would read back
// wrong in the NEXT process, i.e. after the source has already been removed. It never needed a sync to do
// that job, which is why it stays per-record while the flush does not. status() is the durability gate;
// this line is a serialisation gate. Neither substitutes for the other.
class Batch
{
public:
    explicit Batch(QSettings& s) : s_(s) {}

    // The serialisation gate. false -> the caller skips this record and does NOT queue its source for
    // removal, so both copies survive and the next refresh retries — exactly the old `continue`.
    bool write(const QString& key, const QVariant& value)
    {
        s_.setValue(key, value);
        dirty_ = true;
        return s_.value(key).toString() == value.toString();
    }

    // A DESTINATION key that has to disappear as part of the write itself (resume's stale-leaf clear), not
    // a source being retired. It rides the write flush, so the destination is never on disk half-cleared.
    void clearNow(const QString& key) { s_.remove(key); dirty_ = true; }

    // A SOURCE to retire once the writes are durable.
    void removeLater(const QString& key) { pending_.push_back(key); }

    // THE DURABILITY GATE, once for the pass. Returns false when the writes did not reach disk; the
    // sources are then untouched.
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

private:
    QSettings&  s_;
    QStringList pending_;
    bool        dirty_ = false;
};

// ---- ItemMarks merge ------------------------------------------------------------------------------------
// hidden: OR. Hiding is a deliberate act and un-hiding is one keypress; losing a hide silently repopulates a
//   library the user curated, while a spurious hide is visible and trivially undone.
// completion: a REAL verdict always beats "none" (none means unset, not "not finished"); when both sides
//   carry a real verdict the newer updatedAt wins, because that is the user's most recent statement.
// tags: UNION, destination order first. Tags are additive by nature and dropping one loses a shelf.
// updatedAt: max — the record's newest write is what the multi-device merge orders on.
//
// …WITH ONE EXCEPTION, AND IT IS THE WHOLE OF ISSUE #166. Every rule above is monotone-ADD-ONLY: it can turn
// false into true and it can grow the tag list, and it can never do the reverse. That is right for two LIVE
// records collapsing into one, which is what the rules were written for. It is wrong for the one blob shape
// that means the opposite of a record — an ALL-DEFAULT blob, which since #132 is not "an empty record" but a
// CLEAR: the stamped husk ItemMarks::saveItem leaves where a mark was removed. OR-ing an older marked copy
// into a newer husk un-does the user's clear and re-stamps the result as the newest thing in the fleet, so
// the un-clear then propagates to every device. A clear must not be resurrected by an older record — that is
// #132's rule, and this merge is inside its scope, not outside it.
//
// So: THE NEWER SIDE, IF IT IS A CLEAR, WINS OUTRIGHT. Both directions, because both are reachable:
//   * destination newer — this device remapped, the user then cleared the combined tile, and a peer that has
//     not remapped re-imported the old row on the next merge (a retired row is DELETED, see remapMarks, so
//     an absence reads as ignorance and the peer's copy comes back). The next library refresh would fold the
//     stale copy back in.
//   * SOURCE newer — the mirror image: a peer that has not remapped cleared the mark under the OLD id, so
//     what arrives here under that id is that peer's husk, and the record this device already moved to the
//     combined id is the stale one.
// It is a pure function of the two blobs and of nothing else, which is the property that matters: every
// device reaches the same verdict whatever order they ran the repair in. A device-local ledger of "what this
// device already absorbed" would fix the first case and not the second — the peer has no such ledger — and
// would make the outcome depend on which device ran the repair first, which is exactly what #166 asks not to
// happen.
//
// EQUAL stamps fall through to the ordinary rules deliberately: at equal stamps neither side is "the more
// recent statement", so there is nothing for a clear to be newer THAN, and the union is the order-independent
// answer. RESIDUAL, stated rather than hidden: a PARTIAL clear (one tag removed, the hide kept) is not
// distinguishable from "this record never carried that tag" by any function of the two blobs, because marks
// carry one stamp per RECORD and not per field — so an older peer copy can still restore a single retired
// tag until that peer remaps too. Closing that needs per-field stamps in ItemMarks, not a rule here.
bool isClearedMarks(const QJsonObject& o)
{
    // The same predicate as ItemMarks' isDefault (ItemMarks.cpp:82), read off the blob: no hide, no verdict,
    // no tags. Deliberately ignores updatedAt, exactly as that one does — a husk IS an all-default blob.
    const QString c = o.value(QStringLiteral("completion")).toString();
    return !o.value(QStringLiteral("hidden")).toBool()
        && (c.isEmpty() || c == QLatin1String("none"))
        && o.value(QStringLiteral("tags")).toArray().isEmpty();
}

QString clearedBlob(qint64 updatedAt)
{
    QJsonObject o;
    o.insert(QStringLiteral("hidden"), false);
    o.insert(QStringLiteral("completion"), QStringLiteral("none"));
    o.insert(QStringLiteral("tags"), QJsonArray());
    o.insert(QStringLiteral("updatedAt"), double(updatedAt));
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QString mergeCompletion(const QString& a, qint64 ta, const QString& b, qint64 tb)
{
    const bool aNone = a.isEmpty() || a == QLatin1String("none");
    const bool bNone = b.isEmpty() || b == QLatin1String("none");
    if (aNone) return bNone ? QStringLiteral("none") : b;
    if (bNone) return a;
    return (ta >= tb) ? a : b;
}

QString mergeMarks(const QString& dstJson, const QString& srcJson)
{
    const QJsonObject a = QJsonDocument::fromJson(dstJson.toUtf8()).object();
    const QJsonObject b = QJsonDocument::fromJson(srcJson.toUtf8()).object();
    const qint64 ta = qint64(a.value(QStringLiteral("updatedAt")).toDouble());
    const qint64 tb = qint64(b.value(QStringLiteral("updatedAt")).toDouble());

    // #166: the newer side, if it is a CLEAR, wins outright. See the block comment above.
    if (ta != tb)
    {
        const QJsonObject& newer = (ta > tb) ? a : b;
        if (isClearedMarks(newer)) return clearedBlob(std::max(ta, tb));
    }

    QJsonObject o;
    o.insert(QStringLiteral("hidden"), a.value(QStringLiteral("hidden")).toBool()
                                    || b.value(QStringLiteral("hidden")).toBool());
    o.insert(QStringLiteral("completion"),
             mergeCompletion(a.value(QStringLiteral("completion")).toString(), ta,
                             b.value(QStringLiteral("completion")).toString(), tb));

    QJsonArray tags;
    QStringList seen;
    for (const QJsonObject& side : { a, b })
        for (const QJsonValue& v : side.value(QStringLiteral("tags")).toArray())
        {
            const QString t = v.toString();
            if (t.isEmpty() || seen.contains(t)) continue;
            seen.push_back(t);
            tags.append(t);
        }
    o.insert(QStringLiteral("tags"), tags);
    o.insert(QStringLiteral("updatedAt"), double(std::max(ta, tb)));
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// ---- ConsumptionStats merge -----------------------------------------------------------------------------
// mediaSeconds: SUM. It is a pure accumulator, and the per-category rollup (cat/video/seconds) is maintained
//   as the sum of every item's seconds — so summing is the only rule that leaves the rollup coherent with
//   the items, which the stats probe checks.
// pagesRead: MAX, not sum. Its own writer defines it as the HIGH-WATER page index ever reached
//   (ConsumptionStats.cpp:252 accrues only new ground and never decrements), so adding two of them would
//   invent pages nobody read. (Zero for a PC game either way; the rule is stated so a later reader of this
//   code does not "fix" it into a sum.)
// lastActivity: MAX, and the side that owns it also owns title/category — the same newest-wins-for-display
//   rule ConsumptionStats::ensureCache already applies when it folds two devices' copies of one item.
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

// ---- per-store passes -----------------------------------------------------------------------------------
// Each pass is written the same way: locate the source record, skip if absent, merge into the destination,
// stage the write, and queue the source for removal; the pass then commits ONCE. A `continue` on a failed
// write, or a commit that reports an error, leaves BOTH copies in place — the next refresh retries. Nothing
// here can reach a state where neither copy exists.

// marks/<profile>/items/<md5(id)> -> one JSON blob per item (ItemMarks.h).
//
// THE RETIRED SOURCE IS DELETED, AND IT IS NOT DATED (issue #166). Every OTHER place in the tree that stops
// holding a value for a per-item key leaves a dated husk (#132) or a tombstone (#150), because there
// "cleared" and "never known" must not share a representation. A REMAP IS NEITHER. It is a device-local
// repair, not a statement by the user, so it has nothing true to say about the old key and must not date
// anything: a husk stamped now would be a clear that is newer than every peer's genuine data, and a peer
// that has not run the repair yet — the device still READING that key — would have its real marks deleted by
// a repair it did not run. That is a worse loss than the one #132 fixed, and it is the loss #132's shape
// would cause if it were applied here mechanically.
//
// WHAT A PEER THAT HAS NOT REMAPPED SEES IS THEREFORE: NOTHING. Its rows under the old id stand untouched
// until it runs the repair itself, and that is the decision — not an accident of which device ran the repair
// first. The price of not dating the retirement is that the merge's never-delete pass re-imports the old row
// here on the next sync (an absence has no timestamp, so it reads as ignorance — CloudMerge.cpp's
// remoteReplaces states the rule). That re-import is harmless BECAUSE mergeMarks refuses to let a stale copy
// un-do a newer clear; without that guard the round trip would silently restore a hide the user removed. The
// orphan goes away for good once the peer remaps too.
void remapMarks(QSettings& s, const QHash<QString, QString>& table)
{
    s.beginGroup(QStringLiteral("marks"));
    const QStringList profiles = s.childGroups();
    s.endGroup();

    Batch b(s);
    for (const QString& p : profiles)
    {
        const QString items = QStringLiteral("marks/") + p + QStringLiteral("/items/");
        for (auto it = table.cbegin(); it != table.cend(); ++it)
        {
            if (it.key() == it.value()) continue;              // self-map: nothing to move
            const QString src = items + md5Hex(it.key());
            const QString dst = items + md5Hex(it.value());
            if (src == dst) continue;
            const QString srcBlob = s.value(src).toString();
            if (srcBlob.isEmpty()) continue;                   // no record under the old id
            const QString dstBlob = s.value(dst).toString();
            // Nothing at the destination -> move the blob VERBATIM (keeps updatedAt, which the multi-device
            // merge orders on). Something there -> merge, never overwrite.
            const QString merged = dstBlob.isEmpty() ? srcBlob : mergeMarks(dstBlob, srcBlob);
            if (!b.write(dst, merged)) continue;
            b.removeLater(src);
        }
    }
    b.commit();
}

// stats/<profile>/<device>/items/<md5(id)>, plus the pre-namespacing shape stats/<profile>/items/<md5(id)>
// that ConsumptionStats::migrateStatsProfile folds away. Both are handled: the remap may well run before
// that fold on a machine upgrading from an old build, and a record it failed to see would be orphaned by
// the fold a moment later.
//
// THE ACCUMULATORS ANSWER #166 DIFFERENTLY FROM marks, AND THE DIFFERENCE IS NOT A JUDGEMENT CALL — it is
// what their merge rule already is. marks merge per ROW by newest updatedAt; stats and playstats merge per
// DEVICE NAMESPACE, copied wholesale under a `lastWrite` freshness gate (CloudMerge's mergeNamespaced). A
// per-row husk is therefore not a thing that merge can even compare — the namespace is replaced or kept
// entire — so the two representations #132 and #150 offer are both unavailable here, and deleting the row is
// the only shape there is.
//
// It is also already the right one, for one reason that must not be undone: THIS PASS NEVER WRITES
// `lastWrite`. That leaf is stamped only by the namespace's OWNER at a real accrual, and it is the entire
// freshness gate. A remap that stamped it — including in the foreign namespaces this pass rewrites, which
// this device does not own — would make a repair outrank the owner's genuine data, and mergeNamespaced would
// then hand every peer this device's rekeyed copy in place of the one it is still reading. Leaving the stamp
// alone is what makes the repair invisible to a peer, which is the same answer marks reach by not dating the
// retirement, reached by a different mechanism. probe_cloudmerge §30f is the assertion that says so.
//
// The residual, stated rather than hidden: once the OWNER of a remapped namespace next accrues, its
// lastWrite moves and peers take the rekeyed copy verbatim — so a peer that has not remapped stops seeing
// that device's contribution to the game until it remaps too. Nothing is lost, and it cannot be avoided
// while the rollups stay coherent: leaving the source row standing beside the destination would double the
// item against `cat/` (stats) and against profileTotalSeconds (playstats), which is a wrong number on screen
// rather than a delayed-correct one.
void remapConsumption(QSettings& s, const QHash<QString, QString>& table)
{
    s.beginGroup(QStringLiteral("stats"));
    const QStringList profiles = s.childGroups();
    s.endGroup();

    Batch b(s);
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
            if (c == QLatin1String("items") || c == QLatin1String("cat")) continue; // legacy roots, above
            itemRoots << base + QLatin1Char('/') + c + QStringLiteral("/items/");   // a device namespace
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
                const QString merged = dstBlob.isEmpty() ? srcBlob : mergeStats(dstBlob, srcBlob);
                if (!b.write(dst, merged)) continue;
                b.removeLater(src);
            }
    }
    b.commit();
}

// playstats/<profile>/<device>/<sha1(id)>/{total,sessions,last} — and the pre-namespacing shape
// playstats/<profile>/<sha1(id)>/{...}, told apart exactly the way PlayStats::migratePlayProfile tells them
// apart: a LEGACY game group carries total/last/sessions as DIRECT leaves, a device namespace carries game
// subgroups. total and sessions SUM (both are pure accumulators, and profileTotalSeconds is their sum, so
// summing keeps the profile total exact); last is the MAX (the later of two "last played" stamps is the one
// that is true of the merged game).
//
// THIS IS THE ONLY STORE WHOSE RETRY IS NOT FREE, AND IT NEEDS A JOURNAL.
// Every other pass writes ONE key from values it copies or merges, so re-running it recomputes the same
// answer. This one writes THREE keys from a SUM, and the three writes cannot fail together: if `total`
// (= a+b) lands and `sessions` does not, the pass gives up with the source still in place — correct by rule
// 2, nothing is lost — but the destination now holds a+b while the source still holds b, and the next
// refresh re-sums to a+2b. Rule 4 (idempotence) breaks exactly on the failure path, which is the path
// nobody exercises, and the symptom is inflated play time on every subsequent refresh.
//
// CHOSEN FIX: a per-record journal marker carrying the already-computed ABSOLUTE values, so a retry COMMITS
// them rather than re-deriving them from a destination it may have half-written. (The brief's other option —
// stage every leaf, verify, then write — does not close the window: "then write" is still three writes that
// can fail between each other. The marker is what survives that.) Sequence per record:
//   1. marker absent -> compute the sums and stage the marker.
//   2. marker present (this pass or a previous one) -> commit its values to the three leaves. The values
//      are ABSOLUTE, so committing twice is committing once.
//   3. queue the source AND the marker for removal; a completed move leaves no journal.
// The marker lives under its own top-level group that no store reads, so a stale one cannot be mistaken for
// a record.
//
// THE MARKER IS STILL LOAD-BEARING UNDER BATCHING, for a different half of the problem. It is now staged in
// the SAME flush as the leaves, so it no longer reaches disk strictly first — it does not need to: one flush
// carries the marker and all three leaves or none of them, which is exactly the "cannot fail together"
// hazard the journal was invented for, closed by construction. What the marker still does is carry absolute
// values across a FAILED commit, where this process's in-memory map holds them and the retry must not re-sum
// a destination it already added to in memory. Removing it would re-open rule 4 the moment any future change
// re-splits these flushes, which is a silent, permanent play-time inflation — so it stays.
// Residual, stated rather than hidden: if the destination accrues NEW play from actual gameplay between a
// failed commit and the retry, the commit overwrites that increment. That needs a write error (a full or
// read-only ini) AND a play session in the same window, and it loses one session instead of inflating the
// total on every refresh forever.
QString journalKey(const QString& container, const QString& srcHash, const QString& dstHash)
{
    // Its own namespace, hashed so the key length is bounded. The composition is pinned by probe_pcgames,
    // which rebuilds it independently — a change here that the probe does not know about fails loudly
    // instead of silently orphaning in-flight markers.
    return QStringLiteral("pcgameremap/pending/")
         + md5Hex(container + QLatin1Char('|') + srcHash + QLatin1Char('|') + dstHash);
}

void remapPlayStats(QSettings& s, const QHash<QString, QString>& table)
{
    s.beginGroup(QStringLiteral("playstats"));
    const QStringList profiles = s.childGroups();
    s.endGroup();

    Batch b(s);
    for (const QString& p : profiles)
    {
        const QString base = QStringLiteral("playstats/") + p;
        s.beginGroup(base);
        const QStringList children = s.childGroups();
        s.endGroup();

        auto isLegacyGame = [&s, &base](const QString& g) {
            const QString gk = base + QLatin1Char('/') + g;
            return s.contains(gk + QStringLiteral("/total"))
                || s.contains(gk + QStringLiteral("/last"))
                || s.contains(gk + QStringLiteral("/sessions"));
        };

        QStringList containers;
        containers << base;                                   // holds legacy <sha1> game groups, if any
        for (const QString& c : children)
            if (!isLegacyGame(c)) containers << base + QLatin1Char('/') + c;   // a device namespace

        for (const QString& c : containers)
            for (auto it = table.cbegin(); it != table.cend(); ++it)
            {
                if (it.key() == it.value()) continue;
                const QString srcHash = sha1Hex(it.key());
                const QString dstHash = sha1Hex(it.value());
                const QString src = c + QLatin1Char('/') + srcHash;
                const QString dst = c + QLatin1Char('/') + dstHash;
                if (src == dst) continue;

                const QString jk = journalKey(c, srcHash, dstHash);
                QJsonObject staged = QJsonDocument::fromJson(s.value(jk).toString().toUtf8()).object();

                if (staged.isEmpty())
                {
                    // No marker: this record has never been half-written, so the destination is untouched
                    // and summing it with the source is the right answer.
                    const bool has = s.contains(src + QStringLiteral("/total"))
                                  || s.contains(src + QStringLiteral("/last"))
                                  || s.contains(src + QStringLiteral("/sessions"));
                    if (!has) continue;

                    staged.insert(QStringLiteral("total"),
                                  double(s.value(dst + QStringLiteral("/total"), 0).toLongLong()
                                       + s.value(src + QStringLiteral("/total"), 0).toLongLong()));
                    staged.insert(QStringLiteral("sessions"),
                                  double(s.value(dst + QStringLiteral("/sessions"), 0).toLongLong()
                                       + s.value(src + QStringLiteral("/sessions"), 0).toLongLong()));
                    staged.insert(QStringLiteral("last"),
                                  double(std::max(s.value(dst + QStringLiteral("/last"), 0).toLongLong(),
                                                  s.value(src + QStringLiteral("/last"), 0).toLongLong())));
                    // The marker is staged FIRST, so a serialisation failure on it costs a retry and
                    // nothing else — the leaves below are never reached for this record.
                    if (!b.write(jk,
                                 QString::fromUtf8(QJsonDocument(staged).toJson(QJsonDocument::Compact))))
                        continue;
                }
                // Commit ABSOLUTE values — from the marker, never re-derived from a destination that may
                // already hold some of them. This is what makes the retry after a partial write land on
                // a+b instead of a+2b.
                const qint64 total = qint64(staged.value(QStringLiteral("total")).toDouble());
                const qint64 sess  = qint64(staged.value(QStringLiteral("sessions")).toDouble());
                const qint64 last  = qint64(staged.value(QStringLiteral("last")).toDouble());

                if (!b.write(dst + QStringLiteral("/total"), total))    continue;
                if (!b.write(dst + QStringLiteral("/sessions"), sess))  continue;
                if (last > 0 && !b.write(dst + QStringLiteral("/last"), last)) continue;
                b.removeLater(src);   // the whole game subgroup
                b.removeLater(jk);    // the move is complete; the marker has nothing left to protect
            }
    }
    b.commit();
}

// favorites/<profile>/items -> a JSON array whose entries carry itemId (FavoritesStore.cpp). Not hashed, so
// this is the one store the remap can read straight through. Two collapsing favourites become ONE entry:
// the FIRST in list order wins its display fields (the list is newest-first, so that is the entry the user
// most recently starred) and takes the MAX ts, so the merged star keeps the newer star date and its place.
//
// It also STAMPS the record, not just its id — see the comment on the stamp below. A favourite is the one
// migrated record with a second key beside the id (`system`, which decides which console's ★ Favorites folder
// lists it), and moving the id without it produces a star that is visible on Home and gone from the folder.
void remapFavorites(QSettings& s, const QHash<QString, QString>& table)
{
    s.beginGroup(QStringLiteral("favorites"));
    const QStringList profiles = s.childGroups();
    s.endGroup();

    Batch b(s);
    for (const QString& p : profiles)
    {
        const QString key = QStringLiteral("favorites/") + p + QStringLiteral("/items");
        const QString json = s.value(key).toString();
        if (json.isEmpty()) continue;

        const QJsonArray in = QJsonDocument::fromJson(json.toUtf8()).array();
        QJsonArray out;
        QHash<QString, int> seen;     // itemId -> index in `out`
        bool changed = false;

        for (const QJsonValue& v : in)
        {
            if (!v.isObject()) continue;
            QJsonObject o = v.toObject();
            const QString id = o.value(QStringLiteral("itemId")).toString();
            const QString mapped = table.value(id);
            // Only rewrite when the table actually HAS a non-empty destination. An id the table cannot map
            // keeps its own id; writing table.value()'s default here would blank the favourite's key.
            if (!id.isEmpty() && !mapped.isEmpty() && mapped != id)
            {
                o.insert(QStringLiteral("itemId"), mapped);
                changed = true;
            }
            const QString finalId = o.value(QStringLiteral("itemId")).toString();

            // A favourite on a merged id BELONGS TO THE PC CONSOLE, and both surfaces that show that
            // console's ★ Favorites folder — browse::favoritesCatalog and the hasFav gate in HomeView's
            // populate() — filter on `system`. The legacy per-launcher star was written by the generic
            // addon-favourite path, which stamped neither `system` nor `kind` (there was no path to derive
            // one from), so rewriting ONLY the id migrates a record that Home and Plays show correctly and
            // that folder silently omits: the star disappears from exactly where the user looks for it.
            // This is the same inconsistency commit 542a023 fixed on the WRITE side (browse::
            // localGameFavorite stamps "pc" from a pcgame: id); this is the MIGRATED side of it.
            //
            // Keyed on the FINAL id, not on "did this pass rewrite it": a record migrated by an earlier
            // build already carries a pcgame: id and appears in NO table (the table's keys are per-launcher
            // ids), so a rewrite-only condition would strand precisely the users who ran that build.
            // EMPTY fields only — an existing value is the caller's and is never overwritten.
            if (finalId.startsWith(QStringLiteral("pcgame:")))
            {
                if (o.value(QStringLiteral("system")).toString().isEmpty())
                { o.insert(QStringLiteral("system"), QStringLiteral("pc")); changed = true; }
                // `kind` is the row's routing mime in that folder (favoritesCatalog defaults an empty one to
                // "game"), and localGameFavorite writes "pcgame" for the same record. Stamped for the same
                // reason: the migrated record and the freshly starred one must be the same record.
                if (o.value(QStringLiteral("kind")).toString().isEmpty())
                { o.insert(QStringLiteral("kind"), QStringLiteral("pcgame")); changed = true; }
            }

            const int at = seen.value(finalId, -1);
            if (at < 0) { seen.insert(finalId, out.size()); out.append(o); continue; }

            // Collapse: keep the earlier (newer) entry, but never let it carry the OLDER star date.
            QJsonObject kept = out.at(at).toObject();
            const double ts = std::max(kept.value(QStringLiteral("ts")).toDouble(),
                                       o.value(QStringLiteral("ts")).toDouble());
            kept.insert(QStringLiteral("ts"), ts);
            out.replace(at, kept);
            changed = true;
        }

        if (!changed) continue;
        // No source to retire: this pass REWRITES one key in place, so there is nothing to remove and the
        // rule-2 ordering does not arise. It still goes through the batch so the rewrite rides one flush
        // per pass rather than one per profile.
        b.write(key, QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
    }
    b.commit();
}

// resume/<md5(id) truncated to 10>/{pos,dur,ts,title} — global, not per-profile.
//
// The merge rule here is NEWEST-WINS rather than an accumulation, and that is not a shortcut: a resume
// position is a single point in a single stream, so there is nothing additive about two of them. Summing or
// maxing `pos` across two records would produce a position that was never reached in either. The newer ts
// is the one the user last left off at, which is the same rule CloudMerge already applies to resume/ keys.
// Stated explicitly because it is the one store where "merge" does discard a value.
//
// Retiring the source key records NO resume tombstone, deliberately, and the next person to touch a resume
// deletion should read this before copying #150's shape here. A tombstone says "the user forgot this"; a
// re-key says "the same position now lives under a different id", and dating it would tell every peer to
// forget a position that was not forgotten — including the peer that has not run this migration yet and is
// still the only device holding it. This is the BrandMigration::reconcileAddonRefs posture: a device-local
// repair is not an edit. The cost is that a peer still on the old id re-imports the retired row on the next
// merge, leaving one orphan keyed by an id nothing looks up any more; that is invisible to every reader and
// it goes away once the peer migrates too.
void remapResume(QSettings& s, const QHash<QString, QString>& table)
{
    Batch b(s);
    for (auto it = table.cbegin(); it != table.cend(); ++it)
    {
        if (it.key() == it.value()) continue;
        const QString src = QStringLiteral("resume/") + md5Hex10(it.key());
        const QString dst = QStringLiteral("resume/") + md5Hex10(it.value());
        if (src == dst) continue;

        const bool has = s.contains(src + QStringLiteral("/pos"))
                      || s.contains(src + QStringLiteral("/dur"));
        if (!has) continue;

        const bool dstHas = s.contains(dst + QStringLiteral("/pos"))
                         || s.contains(dst + QStringLiteral("/dur"));
        const qint64 srcTs = s.value(src + QStringLiteral("/ts"), 0).toLongLong();
        const qint64 dstTs = s.value(dst + QStringLiteral("/ts"), 0).toLongLong();

        if (!dstHas || srcTs > dstTs)
        {
            // The winner's record replaces the destination's WHOLESALE — a resume record is one point in
            // one stream, so its leaves only mean anything together. Copying only the leaves the winner
            // HAS would leave the loser's stale ones standing beside them: a newer `pos` of 30 next to an
            // inherited `dur` of 100 renders a 30% resume bar for a position that was never 30% of
            // anything. So a leaf the winner does not carry is CLEARED, not left behind.
            bool ok = true;
            for (const char* leaf : { "/pos", "/dur", "/title" })
            {
                const QString lk = src + QLatin1String(leaf);
                if (s.contains(lk)) ok = b.write(dst + QLatin1String(leaf), s.value(lk)) && ok;
            }
            if (!ok) continue;                       // leave BOTH in place; the next refresh retries
            // clearNow, not removeLater: these are DESTINATION leaves being cleared as part of the write,
            // not sources being retired, so they must ride the same flush as the leaves that replace them.
            for (const char* leaf : { "/pos", "/dur", "/title" })
                if (!s.contains(src + QLatin1String(leaf))) b.clearNow(dst + QLatin1String(leaf));

            // `ts` LAST, deliberately: it is the field the comparison above reads. Written first, a run
            // that then died halfway would leave the destination LOOKING newer than the source, and the
            // retry would skip it and delete the source on top of a half-copied record. (Under batching a
            // half-written destination cannot reach disk at all — one flush carries every leaf or none —
            // but the order is kept: it costs nothing and it is what makes the invariant hold again
            // immediately if these writes are ever split back up.)
            const QString sts = src + QStringLiteral("/ts");
            if (s.contains(sts)) { if (!b.write(dst + QStringLiteral("/ts"), s.value(sts))) continue; }
            else                 b.clearNow(dst + QStringLiteral("/ts"));
        }
        // Either the destination already held the newer position, or we just staged it.
        b.removeLater(src);
    }
    b.commit();
}

} // namespace

#ifdef EB_PCGAMEID_TEST_SEAM
void pcgame::setRemapIniPathForTesting(const QString& path)
{
    delete g_testStore;
    g_testStore   = nullptr;
    g_testIniPath = path;
}

int  pcgame::remapSyncCount()      { return g_syncCount; }
void pcgame::resetRemapSyncCount() { g_syncCount = 0; }
#endif

void pcgame::setRemapCacheInvalidator(std::function<void()> fn)
{
    g_invalidate = std::move(fn);
}

QHash<QString, QString> pcgame::remapTable(const QVector<QPair<QString, QString>>& oldIdToTitle)
{
    QHash<QString, QString> out;
    for (const QPair<QString, QString>& e : oldIdToTitle)
    {
        const QString oldId = e.first;
        // The destination is whatever the CATALOG will key this title under — same function, same
        // arguments, no second implementation to drift. See PcGameRemap.h. effectiveItemId, not itemId:
        // the catalog groups on the id AFTER the user's merge overrides are applied, so a remap that
        // stopped at the pure base would send every record of an overridden game to an id no tile carries
        // — the exact silent stranding this shared-function rule exists to prevent.
        const QString merged = pcgame::effectiveItemId(e.second);

        // RULE 1. No id, or nothing to group on (itemId returns empty) -> the entry is ABSENT from the
        // table. It is NOT mapped to an empty string: applyRemap would then hash "" and rewrite a real
        // record under the key of every other nameless entry, which is a data-destroying bug wearing a
        // migration's clothes. Callers read membership (contains), and value()'s empty default is a miss,
        // not a destination.
        if (oldId.isEmpty() || merged.isEmpty()) continue;

        // An already-merged id maps to ITSELF and stays in the table. That is what makes the function a
        // fixed point — feed its own output back and nothing moves — which is the property that lets this
        // run on every library refresh instead of once, so a reinstalled game still gets migrated.
        out.insert(oldId, merged);
    }
    return out;
}

void pcgame::applyRemap(const QHash<QString, QString>& table)
{
    if (table.isEmpty()) return;

    // Guard the table itself before touching a single record. remapTable cannot emit an empty destination,
    // but applyRemap is a public entry point taking a caller-built hash, and ONE empty value here would
    // hash "" and fuse every affected record onto a single bogus key. Dropping the bad pairs is the only
    // safe reading: an entry with no destination is an entry that does not move.
    QHash<QString, QString> safe;
    for (auto it = table.cbegin(); it != table.cend(); ++it)
        if (!it.key().isEmpty() && !it.value().isEmpty() && it.key() != it.value())
            safe.insert(it.key(), it.value());
    if (safe.isEmpty()) return;

    QSettings& s = store();
    remapMarks(s, safe);
    remapConsumption(s, safe);
    remapPlayStats(s, safe);
    remapFavorites(s, safe);
    remapResume(s, safe);
    // Each pass commits its own writes, so by here the ini is already flushed; this is the belt-and-braces
    // that guarantees the invalidator below never fires over an unflushed file, whatever a pass does later.
    // It is O(1) in the library, which is the whole point of the change — the per-record flushes are gone.
    syncStore(s);

    // The stores cache their per-profile view keyed on the OLD hashes; the ini they read has just changed
    // underneath them. See the header for why this is a hook rather than a direct call.
    if (g_invalidate) g_invalidate();
}
