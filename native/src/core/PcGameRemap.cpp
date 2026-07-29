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

// ---- write-then-verify ----------------------------------------------------------------------------------
// Rule 2 in one function: a record is only ever removed after its replacement has been written and FLUSHED
// without error. QSettings reports a write failure asynchronously, so "setValue returned" is not evidence of
// anything — status() after sync() is the real gate here, and without it a full-disk or read-only ini would
// turn this migration into a deletion pass.
//
// WHAT THE READ-BACK DOES AND DOES NOT PROVE. It is NOT a disk read: QSettings answers value() out of the
// in-memory map setValue just wrote, so the comparison is close to tautological and proves nothing about
// what reached the file. It is kept for the one thing it does catch — a value whose ini round-trip is not
// identity (a type QSettings serialises to a different string than it was handed), which would read back
// wrong in the NEXT process, i.e. after the source has already been removed. status() is the durability
// gate; this line is a serialisation gate. Neither substitutes for the other.
bool writeVerified(QSettings& s, const QString& key, const QVariant& value)
{
    s.setValue(key, value);
    s.sync();
    if (s.status() != QSettings::NoError) return false;
    return s.value(key).toString() == value.toString();
}

// ---- ItemMarks merge ------------------------------------------------------------------------------------
// hidden: OR. Hiding is a deliberate act and un-hiding is one keypress; losing a hide silently repopulates a
//   library the user curated, while a spurious hide is visible and trivially undone.
// completion: a REAL verdict always beats "none" (none means unset, not "not finished"); when both sides
//   carry a real verdict the newer updatedAt wins, because that is the user's most recent statement.
// tags: UNION, destination order first. Tags are additive by nature and dropping one loses a shelf.
// updatedAt: max — the record's newest write is what the multi-device merge orders on.
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
// verify the write, and only then remove the source. A `continue` on a failed write leaves BOTH copies in
// place — the next refresh retries. Nothing here can reach a state where neither copy exists.

// marks/<profile>/items/<md5(id)> -> one JSON blob per item (ItemMarks.h).
void remapMarks(QSettings& s, const QHash<QString, QString>& table)
{
    s.beginGroup(QStringLiteral("marks"));
    const QStringList profiles = s.childGroups();
    s.endGroup();

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
            if (!writeVerified(s, dst, merged)) continue;
            s.remove(src);
            s.sync();
        }
    }
}

// stats/<profile>/<device>/items/<md5(id)>, plus the pre-namespacing shape stats/<profile>/items/<md5(id)>
// that ConsumptionStats::migrateStatsProfile folds away. Both are handled: the remap may well run before
// that fold on a machine upgrading from an old build, and a record it failed to see would be orphaned by
// the fold a moment later.
void remapConsumption(QSettings& s, const QHash<QString, QString>& table)
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
                if (!writeVerified(s, dst, merged)) continue;
                s.remove(src);
                s.sync();
            }
    }
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
// CHOSEN FIX: a per-record journal marker, written and verified BEFORE any destination leaf is touched.
// (The brief's other option — stage every leaf, verify, then write — does not actually close the window:
// "then write" is still three writes that can fail between each other. The marker is what survives that.)
// The marker holds the already-computed absolute values, so a retry COMMITS them rather than re-deriving
// them from a destination it may have half-written. Sequence per record:
//   1. marker absent -> compute the sums, write+verify the marker. Failure here has touched nothing.
//   2. marker present (this pass or a previous one) -> commit its values to the three leaves. The values
//      are ABSOLUTE, so committing twice is committing once.
//   3. remove the source, then the marker. A death between those two leaves a marker whose commit is a
//      no-op, which the next pass performs and clears.
// The marker lives under its own top-level group that no store reads, so a stale one cannot be mistaken for
// a record. Residual, stated rather than hidden: if the destination accrues NEW play from actual gameplay
// between a failed commit and the retry, the commit overwrites that increment. That needs a write error
// (a full or read-only ini) AND a play session in the same window, and it loses one session instead of
// inflating the total on every refresh forever.
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
                    // The marker goes down FIRST and is verified. Until it is on disk nothing has been
                    // written to the destination, so a failure here costs a retry and nothing else.
                    if (!writeVerified(s, jk,
                                       QString::fromUtf8(QJsonDocument(staged).toJson(QJsonDocument::Compact))))
                        continue;
                    s.sync();
                }
                // Commit ABSOLUTE values — from the marker, never re-derived from a destination that may
                // already hold some of them. This is what makes the retry after a partial write land on
                // a+b instead of a+2b.
                const qint64 total = qint64(staged.value(QStringLiteral("total")).toDouble());
                const qint64 sess  = qint64(staged.value(QStringLiteral("sessions")).toDouble());
                const qint64 last  = qint64(staged.value(QStringLiteral("last")).toDouble());

                if (!writeVerified(s, dst + QStringLiteral("/total"), total))    continue;
                if (!writeVerified(s, dst + QStringLiteral("/sessions"), sess))  continue;
                if (last > 0 && !writeVerified(s, dst + QStringLiteral("/last"), last)) continue;
                s.remove(src);      // the whole game subgroup
                s.remove(jk);       // the move is complete; the marker has nothing left to protect
                s.sync();
            }
    }
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
        writeVerified(s, key, QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
    }
}

// resume/<md5(id) truncated to 10>/{pos,dur,ts,title} — global, not per-profile.
//
// The merge rule here is NEWEST-WINS rather than an accumulation, and that is not a shortcut: a resume
// position is a single point in a single stream, so there is nothing additive about two of them. Summing or
// maxing `pos` across two records would produce a position that was never reached in either. The newer ts
// is the one the user last left off at, which is the same rule CloudMerge already applies to resume/ keys.
// Stated explicitly because it is the one store where "merge" does discard a value.
void remapResume(QSettings& s, const QHash<QString, QString>& table)
{
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
                if (s.contains(lk)) ok = writeVerified(s, dst + QLatin1String(leaf), s.value(lk)) && ok;
            }
            if (!ok) continue;                       // leave BOTH in place; the next refresh retries
            for (const char* leaf : { "/pos", "/dur", "/title" })
                if (!s.contains(src + QLatin1String(leaf))) s.remove(dst + QLatin1String(leaf));

            // `ts` LAST, deliberately: it is the field the comparison above reads. Written first, a run
            // that then died halfway would leave the destination LOOKING newer than the source, and the
            // retry would skip it and delete the source on top of a half-copied record. Written last, an
            // interrupted move still reads as older and is simply redone.
            const QString sts = src + QStringLiteral("/ts");
            if (s.contains(sts)) { if (!writeVerified(s, dst + QStringLiteral("/ts"), s.value(sts))) continue; }
            else                 s.remove(dst + QStringLiteral("/ts"));
        }
        // Either the destination already held the newer position, or we just wrote (and verified) it.
        s.remove(src);
        s.sync();
    }
}

} // namespace

#ifdef EB_PCGAMEID_TEST_SEAM
void pcgame::setRemapIniPathForTesting(const QString& path)
{
    delete g_testStore;
    g_testStore   = nullptr;
    g_testIniPath = path;
}
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
        // arguments, no second implementation to drift. See PcGameRemap.h.
        const QString merged = pcgame::itemId(e.second);

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
    s.sync();

    // The stores cache their per-profile view keyed on the OLD hashes; the ini they read has just changed
    // underneath them. See the header for why this is a hook rather than a direct call.
    if (g_invalidate) g_invalidate();
}
