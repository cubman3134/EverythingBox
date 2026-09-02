#include "CloudMerge.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "Tombstones.h"
#include "ItemMarks.h"
#include "ResumeStore.h"      // issue #150: the resume tombstone namespace, shared with the clear sites
#include "MetaOverrides.h"      // invalidate() after merging the per-item metadata corrections (issue #24)
#include "LaunchOptionsStore.h" // invalidate() after merging the per-game launch overrides (issue #51)
#include "Pad2KeyStore.h"       // invalidate() after merging the per-game pad-to-keyboard records (issue #105)
#include "MissedDismiss.h"      // invalidate() after merging the per-show "you missed" dismissals (issue #25)
#include "FilterPresetStore.h"  // issue #184: syncIdForName() for back-filling a legacy preset's stable merge id
#include "ConsumptionStats.h"   // invalidate() after a namespaced-accumulator merge (mdsync T3)
#include "Settings.h"           // deviceId() — never clobber our own accumulator namespace on merge
#include "StoredUrl.h"          // issue #200: a peer on an older build can still send us a signed url
#include "StoredIdentity.h"     // issue #203: ...and a playlist whose rows still ARE signed urls
#include "LiveTvIdentity.h"     // issue #203: ...and the Live TV half of the same question

#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <algorithm>

// Shares the portable everythingbox.ini with every other store (same AppPaths::dataDir() posture). Coherence
// with the store front-ends comes from every writer calling sync(); QSettings reloads on access when the
// on-disk file changed.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

namespace {

// ---- small json helpers -----------------------------------------------------------------------------------

// Canonical serialized bytes of a JSON object (QJsonObject stores keys sorted, so Compact output is stable and
// device-independent). The order-independent equal-timestamp tie-break (mdsync T3, carried from the T2 review)
// compares these bytes lexically.
QByteArray canon(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

// ---- the tie-break key: canon(), modulo an add-on id's SPELLING (#58 review) --------------------------------
//
// An add-on that the brand migration renamed is the SAME add-on under two names, and the stored references to
// it (favourites' per-item addonId, playlists' per-entry addonId) can carry either. The device-local repair
// that reunites them with the id actually loaded — BrandMigration::reconcileAddonRefs — deliberately does NOT
// re-date the blob: a repair is not an edit, and stamping it now would let it beat a genuinely newer change
// made on another device. It also fires no store change-hook, so it arms no push.
//
// Both of those are right, and together they land the repaired blob HERE, at an EQUAL timestamp against the
// unrepaired copy still in the cloud document, differing in nothing but that spelling. Deciding such a tie on
// raw bytes decides it on the spelling — and the previous namespace sorts GREATER than the current one, so it
// won every tie: the first merge after every launch reverted the repair, the favourite went back to reporting
// that its source add-on isn't available, and any later push uploaded the reverted blob, so the cloud copy
// never converged either. Whole-object newest-wins made it worse for playlists — the entire repaired playlist
// went back.
//
// Normalizing the spelling for the COMPARISON says what is actually true: this is not a content difference.
// Two blobs that differ only in it now compare EQUAL, remoteReplaces answers "no", and each device keeps the
// spelling that resolves ON IT. Order-independence is preserved in the sense that matters — A-merges-B and
// B-merges-A leave the two ends semantically identical, and a second round changes nothing.
//
// Only the value of a member literally named "addonId" is normalized, reached recursively (a playlist's
// entries are two levels down), and only ever for the comparison — nothing written back is touched, so a
// malformed or third-party id is still stored byte-verbatim. That set is exactly what reconcileAddonRefs can
// move. A playlist's "legacyKey" also opens with an add-on id and is deliberately NOT repaired, so folding it
// in here would make blobs tie that no repair can ever reconcile.
void normalizeAddonIds(QJsonObject& o);

void normalizeAddonIds(QJsonArray& a)
{
    for (int i = 0; i < a.size(); ++i)
    {
        if (a.at(i).isObject())     { QJsonObject c = a.at(i).toObject(); normalizeAddonIds(c); a.replace(i, c); }
        else if (a.at(i).isArray()) { QJsonArray  c = a.at(i).toArray();  normalizeAddonIds(c); a.replace(i, c); }
    }
}

void normalizeAddonIds(QJsonObject& o)
{
    static const QString oldNs = QString::fromLatin1(AppBrand::Legacy::kAddonPrefix);
    static const QString newNs = QString::fromLatin1(AppBrand::kAddonPrefix);
    const QStringList keys = o.keys();
    for (const QString& k : keys)
    {
        const QJsonValue v = o.value(k);
        if (k == QLatin1String("addonId") && v.isString())
        {
            const QString id = v.toString();
            if (id.startsWith(oldNs)) o.insert(k, newNs + id.mid(oldNs.size()));
        }
        else if (v.isObject()) { QJsonObject c = v.toObject(); normalizeAddonIds(c); o.insert(k, c); }
        else if (v.isArray())  { QJsonArray  c = v.toArray();  normalizeAddonIds(c); o.insert(k, c); }
    }
}

QByteArray tieKey(const QJsonObject& o) { QJsonObject c = o; normalizeAddonIds(c); return canon(c); }

// THE REPRESENTATION RULE EVERY STORE MERGED THROUGH HERE HAS TO KEEP (issues #24, #132). This function can
// only compare two records; it is never handed the reason one of them is missing. Every per-item pass below
// is therefore a NEVER-DELETE pass — a hash absent locally is imported, not treated as a deletion — because
// an absence has no timestamp and could equally mean "cleared a second ago" or "this device has never seen
// that item". So a store whose row can return to an all-default state must NOT remove it: it leaves a stamped
// HUSK (an empty record with a fresh timestamp), which is a clear this function CAN compare and which wins.
// In a store that merges by timestamp, "cleared" and "never known" must not have the same representation.
// The stores that instead represent a deletion as a Tombstone (favourites, playlists, tag vocab, pinned tags,
// and since #150 resume and recents) are the other legal answer — a deletion with a time on it, in its own
// namespace — but they buy boundedness with Tombstones::compact(30), so they can only be used where losing a
// deletion after 30 dormant days is acceptable. Anything else, and the merge below silently undoes the user's
// clear.
//
// WHICH OF THE TWO a store gets is not a matter of taste, and #150 is where the rule got its second half. Ask
// two questions. (1) Are clears bounded by deliberate user actions? A husk is kept for ever, so a store whose
// clears fire on their own — resume clears on every finished episode — would grow its husks with USAGE and
// carry every one in the sync document for ever; that store needs the bounded shape. (2) Does the record have
// a natural shelf life? compact(30) is the price of boundedness: a device dormant 31 days comes back holding a
// record whose deletion has expired, and resurrects it. A month-old playback position is stale anyway, so
// resume can pay that; a hide/complete/tag is a deliberate statement with no expiry, so marks cannot — which
// is exactly why #132 went the other way on the SAME defect. Answer "yes, bounded by the user" and "no, never
// expires" and you want a husk; answer the other way and you want a tombstone.
//
// Should the remote value replace the local one? Newest timestamp wins; on EQUAL timestamps a deterministic
// ORDER-INDEPENDENT decision — the lexically-greater tie key — so A-merges-B and B-merges-A pick the SAME
// winner (values identical up to an add-on id's spelling compare equal -> no replace -> a semantic no-op).
// This uniform rule supersedes the divergent legacy ties (four stores kept-local, recents kept-remote via
// `>=`); that legacy recents byte behaviour is DELIBERATELY superseded here.
bool remoteReplaces(qint64 remoteTs, qint64 localTs, const QJsonObject& remote, const QJsonObject& local)
{
    if (remoteTs != localTs) return remoteTs > localTs;
    return tieKey(remote) > tieKey(local); // equal ts -> order-independent value tie-break
}

QJsonArray stringsToArray(const QStringList& list)
{
    QJsonArray a;
    for (const QString& s : list) a.append(s);
    return a;
}

QStringList arrayToStrings(const QJsonArray& a)
{
    QStringList out;
    for (const QJsonValue& v : a)
    {
        const QString s = v.toString();
        if (!s.isEmpty() && !out.contains(s)) out.push_back(s);
    }
    return out;
}

// The profile ids present under a store: the union of its data groups and its tombstone groups (a profile that
// has ONLY deletions — every item removed — must still be serialized so its tombstones propagate).
QStringList profilesFor(const QString& dataRoot)
{
    QSet<QString> ids;
    QSettings& s = store();
    s.beginGroup(dataRoot);
    for (const QString& g : s.childGroups()) ids.insert(g);
    s.endGroup();
    s.beginGroup(QStringLiteral("deleted/") + dataRoot);
    for (const QString& g : s.childGroups()) ids.insert(g);
    s.endGroup();
    QStringList out(ids.begin(), ids.end());
    out.sort();
    return out;
}

// A store's tombstones as a JSON array [{key,ts}] for the document.
QJsonArray tombsToArray(const QString& tombStore)
{
    QJsonArray a;
    for (const Tombstones::Entry& e : Tombstones::all(tombStore))
    {
        QJsonObject o;
        o.insert(QStringLiteral("key"), e.key);
        o.insert(QStringLiteral("ts"), static_cast<double>(e.ts));
        a.append(o);
    }
    return a;
}

// Merge local + remote tombstones for a store into key->newest-ts, and IMPORT each into the local store at its
// faithful ts (record() never downgrades) so this device re-propagates the peer's deletion. Returns the map for
// the caller's suppression pass.
QHash<QString, qint64> mergeTombs(const QString& tombStore, const QJsonArray& remote)
{
    QHash<QString, qint64> map;
    for (const Tombstones::Entry& e : Tombstones::all(tombStore))
        if (e.ts > map.value(e.key, 0)) map.insert(e.key, e.ts);
    for (const QJsonValue& v : remote)
    {
        const QJsonObject o = v.toObject();
        const QString key = o.value(QStringLiteral("key")).toString();
        const qint64 ts = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
        if (key.isEmpty() || ts <= 0) continue;
        if (ts > map.value(key, 0)) map.insert(key, ts);
        Tombstones::record(tombStore, key, ts); // faithful ts; no-op if a newer local one already exists
    }
    return map;
}

// ---- resume / recent (moved VERBATIM from MainWindow, semantics unchanged) ---------------------------------

void serializeResumeRecent(QJsonObject& resume, QJsonObject& recent)
{
    for (const QString& key : store().allKeys())
    {
        if (key.startsWith(QStringLiteral("resume/")))
        {
            const QString rest = key.mid(7);       // "<hash>/<field>"
            const int slash = rest.indexOf(QLatin1Char('/'));
            if (slash <= 0) continue;
            const QString hash = rest.left(slash), field = rest.mid(slash + 1);
            QJsonObject e = resume.value(hash).toObject();
            if      (field == QStringLiteral("pos"))   e.insert(field, store().value(key).toDouble());
            else if (field == QStringLiteral("dur"))   e.insert(field, store().value(key).toDouble());
            else if (field == QStringLiteral("ts"))    e.insert(field, store().value(key).toDouble());
            else if (field == QStringLiteral("title")) e.insert(field, store().value(key).toString());
            resume.insert(hash, e);
        }
        else if (key.startsWith(QStringLiteral("recent/"))) // "recent/<profile>/items" -> the list JSON string
        {
            recent.insert(key.mid(7), store().value(key).toString());
        }
    }
}

// A resume clear is a TOMBSTONE, not a husk (issue #150). The pass below still never deletes on ABSENCE — an
// absent hash is imported, because an absence carries no timestamp — so a clear has to arrive as a dated
// record of its own, and this store's is a tombstone in the "resume" namespace rather than the stamped husk
// #132 gave marks. Both are legal answers to the representation rule stated at remoteReplaces; which one a
// store gets turns on whether its clears are bounded by deliberate user actions and whether the record has a
// natural shelf life. A resume clear fires on EVERY finished episode, so husks would grow with playback and
// ride the document for ever, while compact(30) costs only a position that a peer dormant for 31 days brings
// back — and a month-old playback point is stale anyway. A mark is the mirror image on both counts.
//
// The tombstone namespace is GLOBAL (no profile leaf), matching resume/*'s own global keying, and its key is
// the <hash> this document is already indexed by — both read from ResumeStore, which the clear sites also read
// from, so the merge and the writers cannot drift apart on the spelling.
void mergeResume(const QJsonObject& resume, const QJsonArray& remoteTombs)
{
    // Merge + IMPORT the peer's clears first, so they are in hand for both passes below and so this device
    // re-propagates them (mergeTombs records each at its faithful ts).
    const QHash<QString, qint64> tombs = mergeTombs(ResumeStore::tombStore(), remoteTombs);

    // For each item, keep whichever position was saved more recently (ts). Never delete a local entry. On an
    // EQUAL ts, the order-independent value tie-break decides (below), replacing the old keep-local-on-tie.
    for (auto it = resume.begin(); it != resume.end(); ++it)
    {
        const QJsonObject re = it.value().toObject();
        const QString prefix = QStringLiteral("resume/") + it.key() + QLatin1Char('/');
        // Deliberately NO tombstone check in this loop. One was written first and then removed: whatever it
        // wrote through, the sweep below re-examined at the same stamp and removed again, so no mutation of it
        // could ever be observed — favourites needs its check inline because it BUILDS the surviving list here,
        // while resume rows are individual keys the sweep can revisit. One mechanism, not two that must agree.
        const bool haveLocal = store().contains(prefix + QStringLiteral("pos"));
        if (haveLocal)
        {
            const qint64 localTs  = static_cast<qint64>(store().value(prefix + QStringLiteral("ts"), 0.0).toDouble());
            const qint64 remoteTs = static_cast<qint64>(re.value(QStringLiteral("ts")).toDouble());
            // Rebuild the local entry in the SAME shape serializeResumeRecent emits, so its canonical bytes
            // match this device's serialized form (a prerequisite for order-independence).
            QJsonObject localObj;
            if (store().contains(prefix + QStringLiteral("pos")))   localObj.insert(QStringLiteral("pos"),   store().value(prefix + QStringLiteral("pos")).toDouble());
            if (store().contains(prefix + QStringLiteral("dur")))   localObj.insert(QStringLiteral("dur"),   store().value(prefix + QStringLiteral("dur")).toDouble());
            if (store().contains(prefix + QStringLiteral("ts")))    localObj.insert(QStringLiteral("ts"),    store().value(prefix + QStringLiteral("ts")).toDouble());
            if (store().contains(prefix + QStringLiteral("title"))) localObj.insert(QStringLiteral("title"), store().value(prefix + QStringLiteral("title")).toString());
            if (!remoteReplaces(remoteTs, localTs, re, localObj)) continue;
        }
        if (re.contains(QStringLiteral("pos")))   store().setValue(prefix + QStringLiteral("pos"),   re.value(QStringLiteral("pos")).toDouble());
        if (re.contains(QStringLiteral("dur")))   store().setValue(prefix + QStringLiteral("dur"),   re.value(QStringLiteral("dur")).toDouble());
        if (re.contains(QStringLiteral("ts")))    store().setValue(prefix + QStringLiteral("ts"),    re.value(QStringLiteral("ts")).toDouble());
        // The title is the one field of a resume row kept in the clear, and a peer running a build older than
        // #200 still derives it with completeBaseName() — which for a stream url is a slice of the query. So
        // it is scrubbed ON THE WAY IN: this device's own ini is the thing being protected, and it has no say
        // over what a peer writes. (The local writer scrubs too; this is the other entrance, not a repeat.)
        if (re.contains(QStringLiteral("title")))
            store().setValue(prefix + QStringLiteral("title"),
                             StoredUrl::label(re.value(QStringLiteral("title")).toString()));
    }

    // THE suppression pass, over the local rows — which by now include anything the loop above just wrote, so
    // it covers a tombstoned remote position and a tombstoned local one in the same stroke. It is also the
    // only half that can carry a peer's clear TO this device: a peer that finished the episode sends a
    // tombstone and NO resume entry for that hash, so there is nothing above to suppress and the local row has
    // to be dropped here or the two devices never converge.
    //
    // `>=`, matching favourites and playlists: a position stamped in the same second as the clear is suppressed
    // (you cannot finish what you never saved, so at an equal stamp the clear is the later event), while a
    // strictly-newer position wins outright — a clear is not a ban, and re-watching works.
    for (auto t = tombs.begin(); t != tombs.end(); ++t)
    {
        const QString prefix = QStringLiteral("resume/") + t.key() + QLatin1Char('/');
        if (!store().contains(prefix + QStringLiteral("pos")) && !store().contains(prefix + QStringLiteral("ts"))
            && !store().contains(prefix + QStringLiteral("dur")) && !store().contains(prefix + QStringLiteral("title")))
            continue;                                     // nothing here to suppress (the ordinary case)
        const qint64 localTs = static_cast<qint64>(store().value(prefix + QStringLiteral("ts"), 0.0).toDouble());
        if (t.value() >= localTs) store().remove(QStringLiteral("resume/") + t.key());
    }
}

// The tombstone namespace for one profile's recents (issue #150) — per profile, mirroring the store's own
// namespacing, exactly as favourites and playlists do. RecentStore::remove/clear write here; the cap does not.
QString recentTombStore(const QString& p) { return QStringLiteral("recent/") + p; }

// One recents row, credential-free (issue #200). Rewrites the same four url-shaped fields RecentStore::add
// scrubs on the way in, by the same rules, so a row that arrives from a peer is indistinguishable from one
// written here. Every OTHER field rides through untouched (`QJsonObject o = in;`) — which is how #224's
// saddon/sitem/sroute/stype reach a peer, and why probe_cloudmerge §38c asserts that none of them can
// carry a query. A new field added here is credential-free by argument or it does not belong in this store.
// Spelled over the raw json rather than through RecentItem because this pass never builds one.
QJsonObject scrubRecentRow(const QJsonObject& in)
{
    QJsonObject o = in;
    const QString path = StoredUrl::location(o.value(QStringLiteral("path")).toString());
    const QString key  = StoredUrl::location(o.value(QStringLiteral("key")).toString());
    const QString ttl  = StoredUrl::title(o.value(QStringLiteral("title")).toString(), path);
    const QString thb  = StoredUrl::artwork(o.value(QStringLiteral("thumb")).toString());
    o.insert(QStringLiteral("path"), path);
    if (key.isEmpty()) o.remove(QStringLiteral("key")); else o.insert(QStringLiteral("key"), key);
    if (ttl.isEmpty()) o.remove(QStringLiteral("title")); else o.insert(QStringLiteral("title"), ttl);
    if (thb.isEmpty()) o.remove(QStringLiteral("thumb")); else o.insert(QStringLiteral("thumb"), thb);
    return o;
}

// A peer's recents tombstones, re-keyed by the same rule. A keyless row's identity IS its url, so a removal
// of one arrives filed under the tokenised spelling — which would both write a credential into deleted/* and
// name an entry the scrubbed list no longer contains, so the peer's removal would be silently ignored.
QJsonArray scrubRecentTombs(const QJsonArray& in)
{
    QJsonArray out;
    for (const QJsonValue& v : in)
    {
        QJsonObject o = v.toObject();
        const QString k = o.value(QStringLiteral("key")).toString();
        if (!k.isEmpty()) o.insert(QStringLiteral("key"), StoredUrl::location(k));
        out.append(o);
    }
    return out;
}

void serializeRecentTombs(QJsonObject& out)
{
    for (const QString& p : profilesFor(QStringLiteral("recent")))
    {
        const QJsonArray tombs = tombsToArray(recentTombStore(p));
        if (!tombs.isEmpty()) out.insert(p, tombs);
    }
}

void mergeRecent(const QJsonObject& recent, const QJsonObject& recentTombs)
{
    // Union the local + remote lists per profile by stable identity (key, else path), keeping the newest ts for
    // each, sorted newest-first and capped.
    //
    // Over the union of profiles named by EITHER half: a peer that cleared a profile's whole list sends
    // tombstones and no "recent" entry at all for it (RecentStore::clear removes the key, so nothing is left to
    // serialize), and a pass driven off "recent" alone would never look at that profile — the deletion would
    // arrive and be ignored, which is the bug wearing a different hat.
    // "recent" is keyed by the ini key minus its "recent/" prefix, i.e. "<profile>/items"; "recentTombs" is
    // keyed by the bare profile, like every other tombstone half of the document. Synthesize the data key for a
    // tombs-only profile rather than the other way round, so the data half's iteration is byte-for-byte what it
    // was before this issue.
    QStringList docKeys = recent.keys();
    for (const QString& p : recentTombs.keys())
    {
        const QString k = p + QStringLiteral("/items");
        if (!docKeys.contains(k)) docKeys.push_back(k);
    }
    for (const QString& docKey : docKeys)
    {
        const QString localKey = QStringLiteral("recent/") + docKey;
        const int slash = docKey.indexOf(QLatin1Char('/'));
        const QString profile = slash > 0 ? docKey.left(slash) : docKey;
        // Merge + IMPORT the peer's removals (faithful ts) so this device re-propagates them.
        const QHash<QString, qint64> tombs =
            mergeTombs(recentTombStore(profile), scrubRecentTombs(recentTombs.value(profile).toArray()));
        QHash<QString, QJsonObject> byId;
        // Dedup by id keeping the winner per remoteReplaces (newest ts; equal ts -> greater canonical bytes).
        // The tie-break is order-independent, so which list is ingested first no longer changes the winner
        // (the old `>=` made the second-ingested list win ties — an order-dependent divergence).
        auto ingest = [&byId](const QJsonArray& arr) {
            for (const QJsonValue& v : arr)
            {
                // SCRUBBED BEFORE ITS IDENTITY IS TAKEN (issue #200). A peer on an older build serializes the
                // signed url it played, and this pass writes the row it wins with straight back into the local
                // ini — so a fix confined to RecentStore would be undone by the next sync with any device that
                // has not upgraded. Scrubbing here also keeps the identity consistent: both halves are reduced
                // by the same pure function, so a local row and its tokenised remote twin collapse to ONE
                // entry (newest wins) instead of appearing twice.
                const QJsonObject o = scrubRecentRow(v.toObject());
                const QString id = o.value(QStringLiteral("key")).toString().isEmpty()
                                       ? o.value(QStringLiteral("path")).toString()
                                       : o.value(QStringLiteral("key")).toString();
                if (id.isEmpty()) continue;
                auto cur = byId.constFind(id);
                if (cur == byId.constEnd()
                    || remoteReplaces(static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble()),
                                      static_cast<qint64>(cur.value().value(QStringLiteral("ts")).toDouble()),
                                      o, cur.value()))
                    byId.insert(id, o);
            }
        };
        ingest(QJsonDocument::fromJson(store().value(localKey).toString().toUtf8()).array());          // local first
        ingest(QJsonDocument::fromJson(recent.value(docKey).toString().toUtf8()).array());             // then remote
        QList<QJsonObject> merged = byId.values();
        // Drop what an explicit removal on EITHER device covers, before the cap: a tombstone at or after an
        // entry's own ts suppresses it, a strictly-newer re-open beats it (so a removal is not permanent), and
        // suppressing before the cut means a removed entry never occupies one of the 40 slots. The rule and the
        // `>=` are favourites' — "no tombstone" is an ABSENT key here, never a ts==0 one, so a legacy entry
        // written before recents were stamped (ts==0) is not swept by 0>=0.
        for (int i = merged.size() - 1; i >= 0; --i)
        {
            const QString id = merged[i].value(QStringLiteral("key")).toString().isEmpty()
                                   ? merged[i].value(QStringLiteral("path")).toString()
                                   : merged[i].value(QStringLiteral("key")).toString();
            const auto t = tombs.constFind(id);
            if (t != tombs.constEnd()
                && t.value() >= static_cast<qint64>(merged[i].value(QStringLiteral("ts")).toDouble()))
                merged.removeAt(i);
        }
        // Newest-first; ties broken by canonical bytes so the cap-40 cut is deterministic (order-independent).
        // Deliberately canon() and not tieKey(): tieKey normalizes an "addonId" field, and a recents row has
        // none (RecentStore writes path/title/kind/thumb/key/system/form/ts, plus #224's saddon/sitem/sroute/stype
        // — `saddon` IS an addon manifest id but is not spelled "addonId" and is not a tie input), so
        // normalizing here would be motion with no reachable effect and no mutation could ever kill it.
        std::sort(merged.begin(), merged.end(), [](const QJsonObject& a, const QJsonObject& b) {
            const double at = a.value(QStringLiteral("ts")).toDouble(), bt = b.value(QStringLiteral("ts")).toDouble();
            if (at != bt) return at > bt;
            return canon(a) > canon(b);
        });
        QJsonArray out;
        for (int i = 0; i < merged.size() && i < 40; ++i) out.append(merged[i]); // cap matches RecentStore's
        // An empty result is written as "[]" and NOT removed. Tidying the key away would read identically to
        // every caller (RecentStore::list parses "[]" and an absent key to the same empty list) and no
        // assertion could tell the two apart, so the store keeps the shape it had before #150.
        store().setValue(localKey, QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
    }
}

// ---- marks (items newest-updatedAt; vocab/pinned union-minus-tombstoned) -----------------------------------
// Items are never deleted here, which is why ItemMarks::saveItem leaves a stamped HUSK when an item is cleared
// back to all-default instead of removing the row (issue #132): the husk is a newer record, so it wins this
// pass and carries the clear to every device, where an absent row would have read as ignorance and let a
// peer's stale marked copy win. A husk rides serializeMarks like any other blob — nothing here needs to know
// it is one — and ItemMarks' readers already treat an all-default blob as "no marks".

QString marksItemsGroup(const QString& p) { return QStringLiteral("marks/") + p + QStringLiteral("/items"); }
QString vocabKey(const QString& p)        { return QStringLiteral("marks/") + p + QStringLiteral("/tagVocab"); }
QString pinnedKey(const QString& p)       { return QStringLiteral("marks/") + p + QStringLiteral("/pinnedTags"); }
QString vocabTombStore(const QString& p)  { return QStringLiteral("marks/") + p + QStringLiteral("/tagVocab"); }
QString pinnedTombStore(const QString& p) { return QStringLiteral("marks/") + p + QStringLiteral("/pinnedTags"); }

void serializeMarks(QJsonObject& marks)
{
    for (const QString& p : profilesFor(QStringLiteral("marks")))
    {
        QJsonObject items;
        {
            QSettings& s = store();
            s.beginGroup(marksItemsGroup(p));
            const QStringList hashes = s.childKeys();
            for (const QString& h : hashes)
                items.insert(h, QJsonDocument::fromJson(s.value(h).toString().toUtf8()).object());
            s.endGroup();
        }
        const QStringList vocab  = arrayToStrings(QJsonDocument::fromJson(store().value(vocabKey(p)).toString().toUtf8()).array());
        const QStringList pinned = arrayToStrings(QJsonDocument::fromJson(store().value(pinnedKey(p)).toString().toUtf8()).array());
        const QJsonArray vTombs = tombsToArray(vocabTombStore(p));
        const QJsonArray pTombs = tombsToArray(pinnedTombStore(p));
        if (items.isEmpty() && vocab.isEmpty() && pinned.isEmpty() && vTombs.isEmpty() && pTombs.isEmpty())
            continue;
        QJsonObject po;
        po.insert(QStringLiteral("items"), items);
        po.insert(QStringLiteral("tagVocab"), stringsToArray(vocab));
        po.insert(QStringLiteral("pinnedTags"), stringsToArray(pinned));
        po.insert(QStringLiteral("vocabTombs"), vTombs);
        po.insert(QStringLiteral("pinnedTombs"), pTombs);
        marks.insert(p, po);
    }
}

void mergeMarks(const QJsonObject& marks)
{
    for (auto it = marks.begin(); it != marks.end(); ++it)
    {
        const QString p = it.key();
        const QJsonObject po = it.value().toObject();

        // Items: newest updatedAt wins per hash; never delete.
        const QJsonObject remoteItems = po.value(QStringLiteral("items")).toObject();
        QSettings& s = store();
        for (auto ri = remoteItems.begin(); ri != remoteItems.end(); ++ri)
        {
            const QJsonObject rblob = ri.value().toObject();
            const qint64 rTs = static_cast<qint64>(rblob.value(QStringLiteral("updatedAt")).toDouble());
            const QString ikey = marksItemsGroup(p) + QLatin1Char('/') + ri.key();
            const QByteArray localRaw = s.value(ikey).toString().toUtf8();
            if (!localRaw.isEmpty())
            {
                const QJsonObject lblob = QJsonDocument::fromJson(localRaw).object();
                const qint64 lTs = static_cast<qint64>(lblob.value(QStringLiteral("updatedAt")).toDouble());
                if (!remoteReplaces(rTs, lTs, rblob, lblob)) continue; // equal ts -> value tie-break
            }
            s.setValue(ikey, QString::fromUtf8(canon(rblob)));
        }

        // Tombstones (merged + imported); then vocab/pinned = union MINUS tombstoned.
        const QHash<QString, qint64> vTombs = mergeTombs(vocabTombStore(p),  po.value(QStringLiteral("vocabTombs")).toArray());
        const QHash<QString, qint64> pTombs = mergeTombs(pinnedTombStore(p), po.value(QStringLiteral("pinnedTombs")).toArray());

        QStringList vocab = arrayToStrings(QJsonDocument::fromJson(s.value(vocabKey(p)).toString().toUtf8()).array());
        for (const QString& t : arrayToStrings(po.value(QStringLiteral("tagVocab")).toArray()))
            if (!vocab.contains(t)) vocab.push_back(t);
        QStringList mergedVocab;
        for (const QString& t : vocab)
            if (!vTombs.contains(t)) mergedVocab.push_back(t);        // a deleted tag stays gone
        if (mergedVocab.isEmpty()) s.remove(vocabKey(p));
        else s.setValue(vocabKey(p), QString::fromUtf8(QJsonDocument(stringsToArray(mergedVocab)).toJson(QJsonDocument::Compact)));

        QStringList pinned = arrayToStrings(QJsonDocument::fromJson(s.value(pinnedKey(p)).toString().toUtf8()).array());
        for (const QString& t : arrayToStrings(po.value(QStringLiteral("pinnedTags")).toArray()))
            if (!pinned.contains(t)) pinned.push_back(t);
        QStringList mergedPinned;
        for (const QString& t : pinned)
            if (!vTombs.contains(t) && !pTombs.contains(t)) mergedPinned.push_back(t); // deleted OR unpinned -> no shelf
        if (mergedPinned.isEmpty()) s.remove(pinnedKey(p));
        else s.setValue(pinnedKey(p), QString::fromUtf8(QJsonDocument(stringsToArray(mergedPinned)).toJson(QJsonDocument::Compact)));

        s.sync();
    }
}

// ---- favourites (union by itemId newest-ts + tombstones) ---------------------------------------------------

QString favKey(const QString& p)       { return QStringLiteral("favorites/") + p + QStringLiteral("/items"); }
QString favTombStore(const QString& p)  { return QStringLiteral("favorites/") + p; }

// ---- A LIVE TV FAVOURITE THAT IS STILL A URL NEVER LEAVES THE DEVICE (issue #203) ---------------------------
//
// A favourited IPTV channel USED to be stored as `itemId = "livetv:" + <stream url>`, `path = <stream url>` —
// and an IPTV url is the one this project has already ruled "routinely embeds provider credentials", which is
// why `iptv/*` (the SOURCE list, the same urls) is carved out of the synced bundle by
// CloudSync::isDeviceLocalKey. The favourite shipped the same credential anyway, through this document,
// because favourites are a per-item store and nothing carved them out.
//
// 02a18bd's ANSWER WAS TO REFUSE EVERY `livetv:` ROW, in both directions, and it said what that cost: a
// starred channel stopped syncing. That was containment, not a fix, and it was chosen because at the time
// there was no other identity to move the row onto.
//
// THERE IS ONE NOW. LiveTvIdentity gives a channel a durable, credential-free name — `livetv:<tvg-id>`, or
// `livetv:name:<normalised name>` when the entry carries no EPG id — and the url is resolved from it at open
// time against whatever sources this device has. So the exclusion NARROWS, from "every livetv: row" to
// "livetv: rows whose payload is still a url", which is exactly the set that carries a credential: a row an
// older build wrote, or one this device has not been able to re-identify yet because the source it came from
// is not configured here. Those stay put and stay local, exactly as they do today. Everything else syncs
// again, which is the user-visible win.
//
// Local rows are never removed: this filters what is SENT and what is ACCEPTED, never what is held.
QJsonArray withoutLiveTvUrls(const QJsonArray& in, const QString& idField)
{
    QJsonArray out;
    for (const QJsonValue& v : in)
        if (!LiveTvIdentity::isCredentialShaped(v.toObject().value(idField).toString())) out.append(v);
    return out;
}

void serializeFavorites(QJsonObject& favorites)
{
    for (const QString& p : profilesFor(QStringLiteral("favorites")))
    {
        // Both halves are filtered. A TOMBSTONE carries the deleted row's identity verbatim (Tombstones.h
        // keeps the original key in the value so all() can hand it back), so un-starring a channel would put
        // the same url into the document under a different name — the trap #200 hit with recents tombstones.
        const QJsonArray items = withoutLiveTvUrls(
            QJsonDocument::fromJson(store().value(favKey(p)).toString().toUtf8()).array(),
            QStringLiteral("itemId"));
        const QJsonArray tombs = withoutLiveTvUrls(tombsToArray(favTombStore(p)), QStringLiteral("key"));
        if (items.isEmpty() && tombs.isEmpty()) continue;
        QJsonObject po;
        po.insert(QStringLiteral("items"), items);
        po.insert(QStringLiteral("tombs"), tombs);
        favorites.insert(p, po);
    }
}

void mergeFavorites(const QJsonObject& favorites)
{
    for (auto it = favorites.begin(); it != favorites.end(); ++it)
    {
        const QString p = it.key();
        const QJsonObject po = it.value().toObject();

        // Union local + remote by itemId, newest ts wins.
        QHash<QString, QJsonObject> byId;
        QStringList order; // preserve a stable newest-first order (local first, then remote extras)
        auto ingest = [&](const QJsonArray& arr) {
            for (const QJsonValue& v : arr)
            {
                const QJsonObject o = v.toObject();
                const QString id = o.value(QStringLiteral("itemId")).toString();
                if (id.isEmpty()) continue;
                if (!byId.contains(id)) { byId.insert(id, o); order.push_back(id); }
                else if (remoteReplaces(static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble()),
                                        static_cast<qint64>(byId[id].value(QStringLiteral("ts")).toDouble()),
                                        o, byId[id])) // equal ts -> order-independent value tie-break
                    byId.insert(id, o);
            }
        };
        ingest(QJsonDocument::fromJson(store().value(favKey(p)).toString().toUtf8()).array());
        // The remote half is filtered on the way IN as well as on the way out (#203): a peer running an older
        // build goes on sending Live TV favourites, and accepting them would write the credential into this
        // device's ini — the entrance a send-side filter alone cannot close, exactly as #200 found. The LOCAL
        // half above is unfiltered, so a channel already starred on this device keeps its star.
        ingest(withoutLiveTvUrls(po.value(QStringLiteral("items")).toArray(), QStringLiteral("itemId")));

        const QHash<QString, qint64> tombs = mergeTombs(
            favTombStore(p), withoutLiveTvUrls(po.value(QStringLiteral("tombs")).toArray(), QStringLiteral("key")));

        QJsonArray out;
        for (const QString& id : order)
        {
            const QJsonObject o = byId.value(id);
            const qint64 ts = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
            if (tombs.contains(id) && tombs.value(id) >= ts) continue; // a REAL tombstone (ts>0) beats older/equal; a strictly-newer re-add wins. "No tombstone" must NOT be conflated with a t=0 tombstone, else a legacy ts==0 favourite (no tombstone) would be dropped by 0>=0.
            out.append(o);
        }
        store().setValue(favKey(p), QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
        store().sync();
    }
}

// ---- bookmarks (per profile; union by id newest-ts + tombstones) -------------------------------------------
// Per-book reading bookmarks (issue #136), byte-for-byte the favourites shape: a per-profile {items, tombs}
// sub-document, union by a STABLE id keeping newest ts, a tombstone at-or-after an item's ts suppressing it.
// The identity field is "id" (a bookmark's id is derived from its book+position, BookmarkStore::idFor), so
// two devices that bookmark the same passage converge on ONE row instead of duplicating, and a delete leaves
// a tombstone (BookmarkStore::remove) so a peer's stale copy cannot resurrect it — the #132/#166 rule. All of
// a profile's books share one list; the id carries the bookKey, so no per-book keying is needed here.

QString bmKey(const QString& p)       { return QStringLiteral("bookmarks/") + p + QStringLiteral("/items"); }
QString bmTombStore(const QString& p)  { return QStringLiteral("bookmarks/") + p; }

// ---- audio bookmarks (per profile; union by id newest-ts + tombstones) -------------------------------------
// Per-item audio bookmarks (issue #140), byte-for-byte the reading-bookmarks shape above: a per-profile
// {items, tombs} sub-document, union by a STABLE id keeping newest ts, a tombstone at-or-after an item's ts
// suppressing it. A bookmark's id is derived from its item+position (AudioBookmarkStore::idFor), so two devices
// that bookmark the same passage converge on ONE row, and a delete leaves a tombstone (AudioBookmarkStore::
// remove) so a peer's stale copy cannot resurrect it — the #132/#166 rule. All of a profile's items share one
// list; the id carries the itemKey, so no per-item keying is needed here.

QString abmKey(const QString& p)      { return QStringLiteral("audiobookmarks/") + p + QStringLiteral("/items"); }
QString abmTombStore(const QString& p) { return QStringLiteral("audiobookmarks/") + p; }

void serializeBookmarks(QJsonObject& bookmarks)
{
    for (const QString& p : profilesFor(QStringLiteral("bookmarks")))
    {
        const QJsonArray items = QJsonDocument::fromJson(store().value(bmKey(p)).toString().toUtf8()).array();
        const QJsonArray tombs = tombsToArray(bmTombStore(p));
        if (items.isEmpty() && tombs.isEmpty()) continue;
        QJsonObject po;
        po.insert(QStringLiteral("items"), items);
        po.insert(QStringLiteral("tombs"), tombs);
        bookmarks.insert(p, po);
    }
}

void mergeBookmarks(const QJsonObject& bookmarks)
{
    for (auto it = bookmarks.begin(); it != bookmarks.end(); ++it)
    {
        const QString p = it.key();
        const QJsonObject po = it.value().toObject();

        // Union local + remote by id, newest ts wins (equal ts -> order-independent value tie-break).
        QHash<QString, QJsonObject> byId;
        QStringList order; // stable order (local first, then remote extras), as favourites
        auto ingest = [&](const QJsonArray& arr) {
            for (const QJsonValue& v : arr)
            {
                const QJsonObject o = v.toObject();
                const QString id = o.value(QStringLiteral("id")).toString();
                if (id.isEmpty()) continue;
                if (!byId.contains(id)) { byId.insert(id, o); order.push_back(id); }
                else if (remoteReplaces(static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble()),
                                        static_cast<qint64>(byId[id].value(QStringLiteral("ts")).toDouble()),
                                        o, byId[id]))
                    byId.insert(id, o);
            }
        };
        ingest(QJsonDocument::fromJson(store().value(bmKey(p)).toString().toUtf8()).array());
        ingest(po.value(QStringLiteral("items")).toArray());

        const QHash<QString, qint64> tombs = mergeTombs(bmTombStore(p), po.value(QStringLiteral("tombs")).toArray());

        QJsonArray out;
        for (const QString& id : order)
        {
            const QJsonObject o = byId.value(id);
            const qint64 ts = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
            if (tombs.contains(id) && tombs.value(id) >= ts) continue; // a REAL tombstone (ts>0) beats an older/equal copy; a strictly-newer re-add resurrects. "No tombstone" is an ABSENT key, never ts==0.
            out.append(o);
        }
        store().setValue(bmKey(p), QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
        store().sync();
    }
}

void serializeAudioBookmarks(QJsonObject& audiobookmarks)
{
    for (const QString& p : profilesFor(QStringLiteral("audiobookmarks")))
    {
        const QJsonArray items = QJsonDocument::fromJson(store().value(abmKey(p)).toString().toUtf8()).array();
        const QJsonArray tombs = tombsToArray(abmTombStore(p));
        if (items.isEmpty() && tombs.isEmpty()) continue;
        QJsonObject po;
        po.insert(QStringLiteral("items"), items);
        po.insert(QStringLiteral("tombs"), tombs);
        audiobookmarks.insert(p, po);
    }
}

void mergeAudioBookmarks(const QJsonObject& audiobookmarks)
{
    for (auto it = audiobookmarks.begin(); it != audiobookmarks.end(); ++it)
    {
        const QString p = it.key();
        const QJsonObject po = it.value().toObject();

        // Union local + remote by id, newest ts wins (equal ts -> order-independent value tie-break).
        QHash<QString, QJsonObject> byId;
        QStringList order; // stable order (local first, then remote extras), as favourites/bookmarks
        auto ingest = [&](const QJsonArray& arr) {
            for (const QJsonValue& v : arr)
            {
                const QJsonObject o = v.toObject();
                const QString id = o.value(QStringLiteral("id")).toString();
                if (id.isEmpty()) continue;
                if (!byId.contains(id)) { byId.insert(id, o); order.push_back(id); }
                else if (remoteReplaces(static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble()),
                                        static_cast<qint64>(byId[id].value(QStringLiteral("ts")).toDouble()),
                                        o, byId[id]))
                    byId.insert(id, o);
            }
        };
        ingest(QJsonDocument::fromJson(store().value(abmKey(p)).toString().toUtf8()).array());
        ingest(po.value(QStringLiteral("items")).toArray());

        const QHash<QString, qint64> tombs = mergeTombs(abmTombStore(p), po.value(QStringLiteral("tombs")).toArray());

        QJsonArray out;
        for (const QString& id : order)
        {
            const QJsonObject o = byId.value(id);
            const qint64 ts = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
            if (tombs.contains(id) && tombs.value(id) >= ts) continue; // a REAL tombstone (ts>0) beats an older/equal copy; a strictly-newer re-add resurrects. "No tombstone" is an ABSENT key, never ts==0.
            out.append(o);
        }
        store().setValue(abmKey(p), QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
        store().sync();
    }
}

// ---- playlists (whole-object newest-updatedAt + tombstones) ------------------------------------------------

QString plKey(const QString& p)        { return QStringLiteral("playlists/") + p + QStringLiteral("/items"); }
QString plTombStore(const QString& p)   { return QStringLiteral("playlists/") + p; }

// ---- A LIVE TV PLAYLIST ENTRY IS REWRITTEN ON THE WIRE, NEVER OMITTED (issue #203) --------------------------
//
// The favourites arm above can simply not send a row it cannot make safe: favourites merge per item, so a row
// this device holds back is a row a peer never learns about, and nothing of the peer's is touched. Playlists
// do NOT merge per item — the whole playlist object is the unit and newest-updatedAt takes all of it — so
// dropping one entry from a playlist that then wins the merge DELETES that entry on every other device. That
// is exactly why 02a18bd left this residual alone rather than filtering it.
//
// So the entry travels, with its identity replaced rather than removed. `livetvUnresolved` is the marker that
// says so, and mergePlaylists below is what makes it safe: an entry wearing it may never overwrite a peer's
// playable copy of the same channel. Locally nothing changes — the raw url stays in the ini, because it is
// still the only thing that can play that channel here.
const QLatin1String kUnresolvedMark("livetvUnresolved");

QJsonObject liveTvEntryForWire(const QJsonObject& in)
{
    const QString itemId = in.value(QStringLiteral("itemId")).toString();
    const QString path   = in.value(QStringLiteral("path")).toString();
    const bool live = in.value(QStringLiteral("type")).toString() == QStringLiteral("livetv")
                      || in.value(QStringLiteral("kind")).toString() == QStringLiteral("livetv")
                      || LiveTvIdentity::isLiveTvId(itemId);
    if (!live) return in;
    // A REPAIRED entry is already credential-free in both fields (LiveTvMigrate sets path = the identity), so
    // it goes as it is. Only a row still holding a url is rewritten.
    if (!LiveTvIdentity::isCredentialShaped(itemId) && !path.contains(QStringLiteral("://"))) return in;
    const QString wire = LiveTvIdentity::wireId(in.value(QStringLiteral("title")).toString());
    QJsonObject o = in;
    o.insert(QStringLiteral("itemId"), wire);
    o.insert(QStringLiteral("path"), wire);
    o.insert(kUnresolvedMark, true);
    return o;
}

QJsonArray playlistsForWire(const QJsonArray& in)
{
    QJsonArray outLists;
    for (const QJsonValue& pv : in)
    {
        if (!pv.isObject()) { outLists.append(pv); continue; }
        QJsonObject p = pv.toObject();
        const QJsonArray items = p.value(QStringLiteral("items")).toArray();
        if (items.isEmpty()) { outLists.append(p); continue; }
        QJsonArray outItems;
        for (const QJsonValue& v : items)
            outItems.append(v.isObject() ? QJsonValue(liveTvEntryForWire(v.toObject())) : v);
        p.insert(QStringLiteral("items"), outItems);   // same count, same order, always
        outLists.append(p);
    }
    return outLists;
}

void serializePlaylists(QJsonObject& playlists)
{
    for (const QString& p : profilesFor(QStringLiteral("playlists")))
    {
        const QJsonArray items = playlistsForWire(
            QJsonDocument::fromJson(store().value(plKey(p)).toString().toUtf8()).array());
        const QJsonArray tombs = tombsToArray(plTombStore(p));
        if (items.isEmpty() && tombs.isEmpty()) continue;
        QJsonObject po;
        po.insert(QStringLiteral("items"), items);
        po.insert(QStringLiteral("tombs"), tombs);
        playlists.insert(p, po);
    }
}

void mergePlaylists(const QJsonObject& playlists)
{
    for (auto it = playlists.begin(); it != playlists.end(); ++it)
    {
        const QString p = it.key();
        const QJsonObject po = it.value().toObject();

        // Whole-object union by playlist id, newest updatedAt wins.
        QHash<QString, QJsonObject> byId;
        QStringList order;
        auto ingest = [&](const QJsonArray& arr) {
            for (const QJsonValue& v : arr)
            {
                const QJsonObject o = v.toObject();
                const QString id = o.value(QStringLiteral("id")).toString();
                if (id.isEmpty()) continue;
                if (!byId.contains(id)) { byId.insert(id, o); order.push_back(id); }
                else if (remoteReplaces(static_cast<qint64>(o.value(QStringLiteral("updatedAt")).toDouble()),
                                        static_cast<qint64>(byId[id].value(QStringLiteral("updatedAt")).toDouble()),
                                        o, byId[id])) // equal updatedAt -> order-independent tie-break
                    byId.insert(id, o);
            }
        };
        const QJsonArray localLists = QJsonDocument::fromJson(store().value(plKey(p)).toString().toUtf8()).array();
        ingest(localLists);
        ingest(po.value(QStringLiteral("items")).toArray());

        const QHash<QString, qint64> tombs = mergeTombs(plTombStore(p), po.value(QStringLiteral("tombs")).toArray());

        // ---- WHAT "THE SAME POSITION" MEANS, PINNED (issue #203) -------------------------------------------
        //
        // A `livetvUnresolved` entry says "this device could not name this channel, here is the safe spelling
        // of what it calls itself". If it wins the whole-object merge it would replace a peer's copy of the
        // same channel — which may be a perfectly playable row — with one that cannot play. That is not
        // acceptable even though it deletes nothing, so the winner's entry is repaired back to the local one.
        //
        // THE MATCH IS BY PLAYLIST ID AND THEN BY CHANNEL NAME — LiveTvIdentity::wireId of the entry's title,
        // which is the one credential-free key BOTH sides can compute from a row they hold, whichever spelling
        // that row's identity is in. It is NOT the list index: an edit on either device reorders entries, and
        // an index match would then repair the wrong row. Where a playlist somehow holds two Live TV entries
        // of one name, the FIRST is the local copy taken — the same first-wins tie-break the identity rule and
        // the migration both use.
        //
        // NOTHING IS ADDED AND NOTHING IS REMOVED. Only the payload of an entry that is already in the winning
        // object changes, and only when the local side had something better for that channel. The winner's own
        // order is kept: the newer edit decides where rows sit; this decides only what one of them says.
        QHash<QString, QHash<QString, QJsonObject>> localPlayable;   // playlist id -> wire name -> entry
        for (const QJsonValue& pv : localLists)
        {
            const QJsonObject lp = pv.toObject();
            const QString lid = lp.value(QStringLiteral("id")).toString();
            if (lid.isEmpty()) continue;
            QHash<QString, QJsonObject>& bucket = localPlayable[lid];
            for (const QJsonValue& ev : lp.value(QStringLiteral("items")).toArray())
            {
                const QJsonObject e = ev.toObject();
                const QString eid = e.value(QStringLiteral("itemId")).toString();
                const bool live = e.value(QStringLiteral("type")).toString() == QStringLiteral("livetv")
                                  || e.value(QStringLiteral("kind")).toString() == QStringLiteral("livetv")
                                  || LiveTvIdentity::isLiveTvId(eid);
                if (!live || e.value(kUnresolvedMark).toBool()) continue;
                const QString wire = LiveTvIdentity::wireId(e.value(QStringLiteral("title")).toString());
                if (!bucket.contains(wire)) bucket.insert(wire, e);
            }
        }
        auto repairLiveTv = [&localPlayable](const QString& listId, const QJsonObject& in) {
            const QJsonArray items = in.value(QStringLiteral("items")).toArray();
            if (items.isEmpty() || !localPlayable.contains(listId)) return in;
            const QHash<QString, QJsonObject>& bucket = localPlayable.value(listId);
            // A REPAIR MAY NEVER MINT A SECOND ROW UNDER ONE itemId, and the reason is sharper here than the
            // usual store invariant: StoredIdentity::sweepPlaylists runs on the tail of this merge and DROPS a
            // post-sweep duplicate, so a repair that produced one would hand the very next line a row to
            // delete — the one outcome this whole arm exists to prevent. Two ways it could:
            //   * two entries of the SAME TITLE (two channels a provider named alike) both serialise to one
            //     wire name and would both claim the same local row — so each local row is spent ONCE, and a
            //     second claimant is left as it arrived (a real name identity, distinct from the local id);
            //   * the winner already carries the local row's own id (a third device sent an unrepaired copy),
            //     so the taken set is seeded with what is already in the object.
            // Seeded from the UNMARKED entries only: a marked entry's own id is the wire name, and a local row
            // that already answers to that exact name must still be allowed to replace it.
            QSet<QString> taken;
            for (const QJsonValue& ev : items)
            {
                const QJsonObject e = ev.toObject();
                if (!e.value(kUnresolvedMark).toBool())
                    taken.insert(e.value(QStringLiteral("itemId")).toString());
            }
            bool touched = false;
            QJsonArray out;
            for (const QJsonValue& ev : items)
            {
                const QJsonObject e = ev.toObject();
                if (!e.value(kUnresolvedMark).toBool()) { out.append(ev); continue; }
                const auto hit = bucket.constFind(e.value(QStringLiteral("itemId")).toString());
                if (hit == bucket.constEnd()) { out.append(ev); continue; }
                const QString localId = hit->value(QStringLiteral("itemId")).toString();
                if (taken.contains(localId)) { out.append(ev); continue; }
                taken.insert(localId);
                out.append(*hit);   // the local, playable copy — in the winner's position
                touched = true;
            }
            if (!touched) return in;
            QJsonObject o = in;
            o.insert(QStringLiteral("items"), out);
            return o;
        };

        QJsonArray out;
        for (const QString& id : order)
        {
            const QJsonObject o = byId.value(id);
            const qint64 ts = static_cast<qint64>(o.value(QStringLiteral("updatedAt")).toDouble());
            if (tombs.contains(id) && tombs.value(id) >= ts) continue; // deleted by a REAL tombstone (ts>0) unless a strictly-newer edit resurrects it. Absence of a tombstone must NOT read as t=0, else a legacy updatedAt==0 playlist would be dropped by 0>=0.
            out.append(repairLiveTv(id, o));
        }
        store().setValue(plKey(p), QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
        store().sync();
    }
    // #203: THE ENTRANCE A WRITER-ONLY FIX CANNOT CLOSE. This merge is whole-object newest-wins, so a peer on
    // an older build can push a playlist whose entries are still signed stream urls straight over a cleaned
    // copy — and the winner is written into the local ini above before anything reads it. Re-identify it here,
    // for the same reason mergeRecent scrubs its rows on the way in (#200). Cheap and idempotent: a document
    // that carried nothing tokenised leaves every row byte-identical and the store is never written.
    StoredIdentity::sweepPlaylists();
}

// ---- filter presets (per profile; union by id newest-ts + tombstones) --------------------------------------
// The saved game-library filters (issue #63), synced by #184. Identical shape to favourites — a per-profile
// {items, tombs} sub-document, union by a stable identity keeping newest ts, a tombstone at-or-after an item's
// ts suppressing it — with two spelling differences: the identity field is "id" (favourites' is "itemId"), and
// a legacy #63 row that predates that field is given its deterministic id here (FilterPresetStore::syncIdForName)
// so an untouched preset still syncs and two peers' copies converge instead of duplicating. A rename is an
// id-stable name edit (FilterPresetStore keeps the id), so it folds onto the one row here rather than arriving
// as a delete+add; a delete leaves a tombstone (FilterPresetStore::remove), so a peer's stale copy cannot
// resurrect it — the #132/#166 rule this store was deferred out of #63 to get right.

QString presetKey(const QString& p)       { return QStringLiteral("filterpresets/") + p + QStringLiteral("/items"); }
QString presetTombStore(const QString& p) { return QStringLiteral("filterpresets/") + p; }

// The id a preset merges under: its stored "id", or — for a legacy #63 row that has none — the deterministic
// name-derived id, computed the SAME way FilterPresetStore back-fills it, so the two never disagree.
QString presetId(const QJsonObject& o)
{
    const QString id = o.value(QStringLiteral("id")).toString();
    return id.isEmpty() ? FilterPresetStore::syncIdForName(o.value(QStringLiteral("name")).toString()) : id;
}

void serializePresets(QJsonObject& presets)
{
    for (const QString& p : profilesFor(QStringLiteral("filterpresets")))
    {
        const QJsonArray items = QJsonDocument::fromJson(store().value(presetKey(p)).toString().toUtf8()).array();
        const QJsonArray tombs = tombsToArray(presetTombStore(p));
        if (items.isEmpty() && tombs.isEmpty()) continue;
        QJsonObject po;
        po.insert(QStringLiteral("items"), items);
        po.insert(QStringLiteral("tombs"), tombs);
        presets.insert(p, po);
    }
}

void mergePresets(const QJsonObject& presets)
{
    for (auto it = presets.begin(); it != presets.end(); ++it)
    {
        const QString p = it.key();
        const QJsonObject po = it.value().toObject();

        // Union local + remote by id, newest ts wins (equal ts -> order-independent value tie-break).
        QHash<QString, QJsonObject> byId;
        QStringList order; // stable newest-first order (local first, then remote extras), as favourites
        auto ingest = [&](const QJsonArray& arr) {
            for (const QJsonValue& v : arr)
            {
                const QJsonObject o = v.toObject();
                const QString id = presetId(o);
                if (id.isEmpty()) continue;
                if (!byId.contains(id)) { byId.insert(id, o); order.push_back(id); }
                else if (remoteReplaces(static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble()),
                                        static_cast<qint64>(byId[id].value(QStringLiteral("ts")).toDouble()),
                                        o, byId[id]))
                    byId.insert(id, o);
            }
        };
        ingest(QJsonDocument::fromJson(store().value(presetKey(p)).toString().toUtf8()).array());
        ingest(po.value(QStringLiteral("items")).toArray());

        const QHash<QString, qint64> tombs = mergeTombs(presetTombStore(p), po.value(QStringLiteral("tombs")).toArray());

        QJsonArray out;
        for (const QString& id : order)
        {
            const QJsonObject o = byId.value(id);
            const qint64 ts = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
            if (tombs.contains(id) && tombs.value(id) >= ts) continue; // a REAL tombstone (ts>0) beats an older/equal copy; a strictly-newer edit resurrects. "No tombstone" is an ABSENT key, never ts==0, so a legacy ts==0 preset is not swept by 0>=0.
            out.append(o);
        }
        store().setValue(presetKey(p), QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
        store().sync();
    }
}

// ---- metadata overrides (global, newest-updatedAt per item; a reset is a husk, never a deletion) ------------
// The user's corrections to a wrong scrape (issue #24). GLOBAL, not per profile — a mis-scrape is wrong for
// the whole household — so the shape is a flat { "<hash>": <blob> }, the same shape as resume.
//
// There are no tombstones here and there must not be: MetaOverrides::reset() stores a timestamp-only HUSK
// rather than removing the row, precisely so the reset is a newer record that wins this merge and propagates.
// A deletion would be indistinguishable from "this device never saw that item", and the next merge with a peer
// still holding the stale override would put back the thing the user just reset.

QString overridesItemsGroup() { return QStringLiteral("metaoverrides/items"); }

void serializeMetaOverrides(QJsonObject& out)
{
    QSettings& s = store();
    s.beginGroup(overridesItemsGroup());
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
        out.insert(h, QJsonDocument::fromJson(s.value(h).toString().toUtf8()).object());
    s.endGroup();
}

void mergeMetaOverrides(const QJsonObject& in)
{
    QSettings& s = store();
    for (auto it = in.begin(); it != in.end(); ++it)
    {
        const QJsonObject rblob = it.value().toObject();
        const qint64 rTs = static_cast<qint64>(rblob.value(QStringLiteral("updatedAt")).toDouble());
        const QString ikey = overridesItemsGroup() + QLatin1Char('/') + it.key();
        const QByteArray localRaw = s.value(ikey).toString().toUtf8();
        if (!localRaw.isEmpty())
        {
            const QJsonObject lblob = QJsonDocument::fromJson(localRaw).object();
            const qint64 lTs = static_cast<qint64>(lblob.value(QStringLiteral("updatedAt")).toDouble());
            if (!remoteReplaces(rTs, lTs, rblob, lblob)) continue; // equal ts -> order-independent tie-break
        }
        s.setValue(ikey, QString::fromUtf8(canon(rblob)));
    }
}

// ---- per-game launch overrides (global, newest-updatedAt per item; a clear is a husk) --------------------
// The game's preferred core / standalone emulator / extra args (issue #51). GLOBAL, not per profile — which
// binary a game runs on is a property of the game+hardware, not the viewer — so the shape is a flat
// { "<hash>": <blob> }, byte-for-byte the metaoverrides section above, and for the same reasons: no tombstones
// (LaunchOpts::reset stores a timestamp-only HUSK so the clear is a newer record that wins and propagates; a
// deletion would be resurrected by a peer still holding the stale override), newest-updatedAt wins per hash,
// equal timestamps break on the canonical bytes.

QString launchOptsItemsGroup() { return QStringLiteral("launchopts/items"); }

void serializeLaunchOpts(QJsonObject& out)
{
    QSettings& s = store();
    s.beginGroup(launchOptsItemsGroup());
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
        out.insert(h, QJsonDocument::fromJson(s.value(h).toString().toUtf8()).object());
    s.endGroup();
}

void mergeLaunchOpts(const QJsonObject& in)
{
    QSettings& s = store();
    for (auto it = in.begin(); it != in.end(); ++it)
    {
        const QJsonObject rblob = it.value().toObject();
        const qint64 rTs = static_cast<qint64>(rblob.value(QStringLiteral("updatedAt")).toDouble());
        const QString ikey = launchOptsItemsGroup() + QLatin1Char('/') + it.key();
        const QByteArray localRaw = s.value(ikey).toString().toUtf8();
        if (!localRaw.isEmpty())
        {
            const QJsonObject lblob = QJsonDocument::fromJson(localRaw).object();
            const qint64 lTs = static_cast<qint64>(lblob.value(QStringLiteral("updatedAt")).toDouble());
            if (!remoteReplaces(rTs, lTs, rblob, lblob)) continue; // equal ts -> order-independent tie-break
        }
        s.setValue(ikey, QString::fromUtf8(canon(rblob)));
    }
}

// ---- per-game pad-to-keyboard records (global, newest-updatedAt per item; a clear is a husk) -------------
// Whether pad2key is enabled for a game + its custom profile (issue #105). Byte-for-byte the launchopts section
// above and for the same reasons: GLOBAL (whether a keyboard-only game needs pad2key is a property of the game,
// not the viewer, and a pad→key map is NOT hardware-specific the way a graphics override is — so unlike emugfx
// it DOES sync), no tombstones (Pad2KeyStore::reset writes a timestamp-only HUSK so a clear is a newer record
// that wins and propagates), newest-updatedAt wins per hash, equal timestamps break on the canonical bytes.

QString pad2keyItemsGroup() { return QStringLiteral("pad2key/items"); }

void serializePad2Key(QJsonObject& out)
{
    QSettings& s = store();
    s.beginGroup(pad2keyItemsGroup());
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
        out.insert(h, QJsonDocument::fromJson(s.value(h).toString().toUtf8()).object());
    s.endGroup();
}

void mergePad2Key(const QJsonObject& in)
{
    QSettings& s = store();
    for (auto it = in.begin(); it != in.end(); ++it)
    {
        const QJsonObject rblob = it.value().toObject();
        const qint64 rTs = static_cast<qint64>(rblob.value(QStringLiteral("updatedAt")).toDouble());
        const QString ikey = pad2keyItemsGroup() + QLatin1Char('/') + it.key();
        const QByteArray localRaw = s.value(ikey).toString().toUtf8();
        if (!localRaw.isEmpty())
        {
            const QJsonObject lblob = QJsonDocument::fromJson(localRaw).object();
            const qint64 lTs = static_cast<qint64>(lblob.value(QStringLiteral("updatedAt")).toDouble());
            if (!remoteReplaces(rTs, lTs, rblob, lblob)) continue;   // equal ts -> order-independent tie-break
        }
        s.setValue(ikey, QString::fromUtf8(canon(rblob)));
    }
}

// ---- per-item playback speed (global, newest-updatedAt per item; no tombstones) --------------------------
// The speed a book/podcast is remembered at (issue #140). GLOBAL, not per profile — a narrator's ideal speed
// is a property of the content, not the viewer — so the shape is a flat { "<hash>": <blob> }, byte-for-byte
// the metaoverrides/launchopts sections above, and for the same reasons: no tombstones (there is no "clear
// speed" verb; changing the speed writes a newer record that propagates), newest-updatedAt wins per hash,
// equal timestamps break on the canonical bytes. SpeedStore writes the same "speed/items/<hash>" blob.

QString speedItemsGroup() { return QStringLiteral("speed/items"); }

void serializeSpeed(QJsonObject& out)
{
    QSettings& s = store();
    s.beginGroup(speedItemsGroup());
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
        out.insert(h, QJsonDocument::fromJson(s.value(h).toString().toUtf8()).object());
    s.endGroup();
}

void mergeSpeed(const QJsonObject& in)
{
    QSettings& s = store();
    for (auto it = in.begin(); it != in.end(); ++it)
    {
        const QJsonObject rblob = it.value().toObject();
        const qint64 rTs = static_cast<qint64>(rblob.value(QStringLiteral("updatedAt")).toDouble());
        const QString ikey = speedItemsGroup() + QLatin1Char('/') + it.key();
        const QByteArray localRaw = s.value(ikey).toString().toUtf8();
        if (!localRaw.isEmpty())
        {
            const QJsonObject lblob = QJsonDocument::fromJson(localRaw).object();
            const qint64 lTs = static_cast<qint64>(lblob.value(QStringLiteral("updatedAt")).toDouble());
            if (!remoteReplaces(rTs, lTs, rblob, lblob)) continue; // equal ts -> order-independent tie-break
        }
        s.setValue(ikey, QString::fromUtf8(canon(rblob)));
    }
}

// ---- per-item lyric offset (global, newest-updatedAt per item; no tombstones) -----------------------------
// How far out a track's lyric file runs (issue #142). A community .lrc that leads by half a second leads by
// half a second on every device, because the drift is a property of the FILE that came with the content — so
// this is GLOBAL, not per profile, and the shape is the flat { "<hash>": <blob> } of speed/metaoverrides
// above, for the same reasons: no tombstones (clearing a nudge WRITES 0.0 rather than deleting the row, so a
// peer holding the old value is beaten by a newer record instead of needing a tombstone), newest-updatedAt
// wins per hash, equal timestamps break on the canonical bytes. LyricOffsetStore writes the same
// "lyricoffset/items/<hash>" blob.

QString lyricOffsetItemsGroup() { return QStringLiteral("lyricoffset/items"); }

void serializeLyricOffset(QJsonObject& out)
{
    QSettings& s = store();
    s.beginGroup(lyricOffsetItemsGroup());
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
        out.insert(h, QJsonDocument::fromJson(s.value(h).toString().toUtf8()).object());
    s.endGroup();
}

void mergeLyricOffset(const QJsonObject& in)
{
    QSettings& s = store();
    for (auto it = in.begin(); it != in.end(); ++it)
    {
        const QJsonObject rblob = it.value().toObject();
        const qint64 rTs = static_cast<qint64>(rblob.value(QStringLiteral("updatedAt")).toDouble());
        const QString ikey = lyricOffsetItemsGroup() + QLatin1Char('/') + it.key();
        const QByteArray localRaw = s.value(ikey).toString().toUtf8();
        if (!localRaw.isEmpty())
        {
            const QJsonObject lblob = QJsonDocument::fromJson(localRaw).object();
            const qint64 lTs = static_cast<qint64>(lblob.value(QStringLiteral("updatedAt")).toDouble());
            if (!remoteReplaces(rTs, lTs, rblob, lblob)) continue; // equal ts -> order-independent tie-break
        }
        s.setValue(ikey, QString::fromUtf8(canon(rblob)));
    }
}

// ---- per-item TRACKER LINKS (global, newest-updatedAt per item; no tombstones) ----------------------------
// Which AniList entry a shelf row IS (issue #156). A link is established by answering a prompt, one item at
// a time, and it describes the CONTENT rather than this machine -- so it is GLOBAL, not per profile, and it
// takes the flat { "<hash>": <blob> } shape of speed/lyricoffset above, for the same reasons: newest
// updatedAt wins per hash, equal timestamps break on the canonical bytes.
//
// NO TOMBSTONES, and the reason is worth stating because unlinking really is a deletion in the user's terms:
// TrackerLinks::clear writes a HUSK (an empty mediaId, a fresh updatedAt) instead of removing the row. A
// removed row would simply be re-merged back in from any peer that still held the old link, and the user
// would find an item they unlinked linked again after a sync. The husk beats it on timestamp instead, which
// is the MetaOverrides argument applied to a smaller blob.
//
// THE CREDENTIALS ARE NOT HERE and must never be. tracker/* and trackerstate/* are device-local
// (CloudSync::isDeviceLocalKey); only trackerlink/* rides this document. probe_cloudmerge asserts both.

QString trackerLinkItemsGroup() { return QStringLiteral("trackerlink/items"); }

void serializeTrackerLink(QJsonObject& out)
{
    QSettings& s = store();
    s.beginGroup(trackerLinkItemsGroup());
    const QStringList hashes = s.childKeys();
    for (const QString& h : hashes)
        out.insert(h, QJsonDocument::fromJson(s.value(h).toString().toUtf8()).object());
    s.endGroup();
}

void mergeTrackerLink(const QJsonObject& in)
{
    QSettings& s = store();
    for (auto it = in.begin(); it != in.end(); ++it)
    {
        const QJsonObject rblob = it.value().toObject();
        const qint64 rTs = static_cast<qint64>(rblob.value(QStringLiteral("updatedAt")).toDouble());
        const QString ikey = trackerLinkItemsGroup() + QLatin1Char('/') + it.key();
        const QByteArray localRaw = s.value(ikey).toString().toUtf8();
        if (!localRaw.isEmpty())
        {
            const QJsonObject lblob = QJsonDocument::fromJson(localRaw).object();
            const qint64 lTs = static_cast<qint64>(lblob.value(QStringLiteral("updatedAt")).toDouble());
            if (!remoteReplaces(rTs, lTs, rblob, lblob)) continue; // equal ts -> order-independent tie-break
        }
        s.setValue(ikey, QString::fromUtf8(canon(rblob)));
    }
}

// ---- "you missed" dismissals (issue #25) — per profile, per show, merged by MAX ---------------------------
// The simplest section in this document, and deliberately so: the store it carries is a set of per-show
// high-water marks whose only mutation is being RAISED, so the merge is `max` and nothing else.
//
// It needs none of the machinery every other section here needs, and the reasons are worth stating because
// they are the reasons the store was shaped that way (TraktMissed.h argues it in full):
//
//   * NO TOMBSTONES. A dismissal is never expressed as a removal — 0/absent is "never dismissed" and a
//     positive stamp is "dismissed through it" — so the #132 hazard a tombstone exists to answer, a deleted
//     row reading as "this device never saw that item", has nothing to attach to here.
//   * NO EQUAL-TIMESTAMP TIE-BREAK. `max` is commutative, associative and idempotent, so A-merges-B and
//     B-merges-A land on the same value with no comparator deciding anything, and a second round changes
//     nothing. Equal values are literally equal.
//   * NO HUSKS. Records are COLLECTED instead (MissedDismiss::prune), which is safe precisely because a
//     stamp older than the lookback window can no longer suppress anything on screen — so a peer that still
//     holds a collected row and re-propagates it here does not resurrect anything a user would see. That is
//     the property MetaOverrides' husks can never have, and it is why this one may be swept and that one
//     may not.

QString missedShowsGroup(const QString& profile) { return QStringLiteral("missed/") + profile + QStringLiteral("/shows"); }

void serializeMissed(QJsonObject& out)
{
    for (const QString& p : profilesFor(QStringLiteral("missed")))
    {
        QJsonObject shows;
        QSettings& s = store();
        s.beginGroup(missedShowsGroup(p));
        const QStringList hashes = s.childKeys();
        for (const QString& h : hashes)
        {
            const qint64 v = s.value(h).toString().toLongLong();
            if (v > 0) shows.insert(h, static_cast<double>(v));   // a non-record is not carried
        }
        s.endGroup();
        if (!shows.isEmpty()) out.insert(p, shows);
    }
}

void mergeMissed(const QJsonObject& in)
{
    QSettings& s = store();
    for (auto pit = in.begin(); pit != in.end(); ++pit)
    {
        const QJsonObject shows = pit.value().toObject();
        for (auto it = shows.begin(); it != shows.end(); ++it)
        {
            const qint64 remote = static_cast<qint64>(it.value().toDouble());
            const QString key = missedShowsGroup(pit.key()) + QLatin1Char('/') + it.key();
            // The max IS the whole merge, and it is also the whole of "a remote non-record is ignored":
            // an absent local row reads back as 0, which is the floor, so a remote 0 (or a negative, or a
            // value that was never a number) can never be greater and can never be written. A guard saying
            // so separately would be a line no mutation could distinguish from its absence.
            if (s.value(key).toString().toLongLong() >= remote) continue;   // ours is as new or newer
            s.setValue(key, QString::number(remote));
        }
    }
}

// ---- device-namespaced accumulators (stats / playstats) — union VERBATIM, never arithmetic (mdsync T3) -----
// Shape: rootPrefix/<profile>/<device>/<sub...>  serialized as { "<profile>": { "<device>": { "<sub>": val }}}.
// A device's namespace is ONLY ever written by that device, so on merge each REMOTE namespace is copied
// wholesale (verbatim replace, never arithmetic) and the LOCAL device's own namespace is never touched.
// Repeated merges therefore never double-count (replace, not add).
//
// Per-namespace freshness (mdsync T4, from the T3 review): a peer can carry a STALE copy of a THIRD device's
// namespace, so a blind verbatim replace would let that stale copy downgrade a locally-fresher one. Each owner
// stamps a `lastWrite` scalar leaf in its namespace at accrual (ConsumptionStats/PlayStats), and that stamp
// travels verbatim; mergeNamespaced replaces a foreign namespace only when the incoming lastWrite is strictly
// NEWER than the local copy's (or the local copy is absent) — newest-wins per foreign namespace.

void serializeNamespaced(const QString& rootPrefix, QJsonObject& out)
{
    QSettings& s = store();
    const QString pfx = rootPrefix + QLatin1Char('/');
    for (const QString& key : s.allKeys())
    {
        if (!key.startsWith(pfx)) continue;
        const QString rest = key.mid(pfx.size());          // "<profile>/<device>/<sub...>"
        const int s1 = rest.indexOf(QLatin1Char('/'));
        if (s1 <= 0) continue;
        const int s2 = rest.indexOf(QLatin1Char('/'), s1 + 1);
        if (s2 <= s1) continue;                             // excludes the "<profile>/schema" stamp (one slash)
        const QString profile = rest.left(s1);
        const QString device  = rest.mid(s1 + 1, s2 - s1 - 1);
        const QString sub     = rest.mid(s2 + 1);
        // Defensive: a device id is a UUID, never these legacy-shape leaves — skip un-migrated stats so a
        // pre-migration key can't be mis-serialized under a fake device (startup migrate() normally precludes).
        if (device == QStringLiteral("items") || device == QStringLiteral("cat")) continue;
        QJsonObject prof = out.value(profile).toObject();
        QJsonObject dev  = prof.value(device).toObject();
        dev.insert(sub, s.value(key).toString());
        prof.insert(device, dev);
        out.insert(profile, prof);
    }
}

void mergeNamespaced(const QString& rootPrefix, const QJsonObject& in, const QString& localDevice)
{
    QSettings& s = store();
    for (auto pit = in.begin(); pit != in.end(); ++pit)
    {
        const QString profile = pit.key();
        const QJsonObject devices = pit.value().toObject();
        for (auto dit = devices.begin(); dit != devices.end(); ++dit)
        {
            const QString device = dit.key();
            if (device == localDevice) continue;            // never clobber our own live namespace
            const QString base = rootPrefix + QLatin1Char('/') + profile + QLatin1Char('/') + device;
            const QJsonObject ns = dit.value().toObject();

            // Freshness gate: keep a locally-fresher copy of this foreign namespace. lastWrite is stored as a
            // decimal-string scalar leaf (owner-stamped, then carried verbatim), so read both via toLongLong.
            const qint64 remoteLW = ns.value(QStringLiteral("lastWrite")).toString().toLongLong();
            s.beginGroup(base);
            const bool localPresent = !s.childKeys().isEmpty() || !s.childGroups().isEmpty();
            s.endGroup();
            if (localPresent)
            {
                const qint64 localLW = s.value(base + QStringLiteral("/lastWrite")).toString().toLongLong();
                if (remoteLW <= localLW) continue;          // our copy is as-fresh-or-fresher -> keep it
            }

            s.remove(base);                                 // verbatim replace: drop the stale copy first
            for (auto kit = ns.begin(); kit != ns.end(); ++kit)
                s.setValue(base + QLatin1Char('/') + kit.key(), kit.value().toString());
        }
    }
}

} // namespace

void CloudMerge::serializeAll(QJsonObject& root)
{
    QJsonObject resume, recent, recentTombs, marks, favorites, bookmarks, audiobookmarks, playlists, presets, stats, playstats, metaoverrides, launchopts, pad2key, speed, lyricoffset, trackerlink, missed;
    serializeResumeRecent(resume, recent);
    serializeRecentTombs(recentTombs);                           // issue #150: the explicit removals
    serializeMarks(marks);
    serializeFavorites(favorites);
    serializeBookmarks(bookmarks);                               // issue #136: per-book reading bookmarks
    serializeAudioBookmarks(audiobookmarks);                     // issue #140: per-item audio bookmarks
    serializePlaylists(playlists);
    serializePresets(presets);                                   // issue #184: saved filter presets
    serializeMetaOverrides(metaoverrides);                       // per-item metadata corrections (issue #24)
    serializeLaunchOpts(launchopts);                             // per-game launch overrides (issue #51)
    serializePad2Key(pad2key);                                   // per-game pad-to-keyboard records (issue #105)
    serializeSpeed(speed);                                       // per-item playback-speed memory (issue #140)
    serializeLyricOffset(lyricoffset);                           // per-item lyric offset memory (issue #142)
    serializeTrackerLink(trackerlink);                           // per-item tracker links (issue #156)
    serializeMissed(missed);                                     // "you missed" dismissals (issue #25)
    serializeNamespaced(QStringLiteral("stats"), stats);         // device-namespaced accumulators (mdsync T3)
    serializeNamespaced(QStringLiteral("playstats"), playstats);
    root.insert(QStringLiteral("resume"), resume);
    root.insert(QStringLiteral("recent"), recent);
    // The two deletion namespaces #150 added, carried as SEPARATE root keys rather than by re-shaping "resume"
    // / "recent" into {items,tombs}. That is what makes the mixed-version fleet work in the direction that
    // cannot be fixed later: an already-shipped build reads root["resume"] as a flat hash->object map and
    // root["recent"]'s per-profile value as the list JSON STRING, so re-shaping either would have made every
    // old device read an empty document and stop merging progress at all. Unknown root keys are ignored by
    // every build (mergeAll reads by name), so these ride along invisibly until the peer is upgraded.
    root.insert(QStringLiteral("resumeTombs"), tombsToArray(ResumeStore::tombStore()));
    root.insert(QStringLiteral("recentTombs"), recentTombs);
    root.insert(QStringLiteral("marks"), marks);
    root.insert(QStringLiteral("favorites"), favorites);
    root.insert(QStringLiteral("bookmarks"), bookmarks);         // issue #136 — a new root key; old builds ignore it (mergeAll reads by name)
    root.insert(QStringLiteral("audiobookmarks"), audiobookmarks); // issue #140 — a new root key; old builds ignore it (mergeAll reads by name)
    root.insert(QStringLiteral("playlists"), playlists);
    root.insert(QStringLiteral("presets"), presets);             // issue #184 — a new root key; old builds ignore it (mergeAll reads by name)
    root.insert(QStringLiteral("metaoverrides"), metaoverrides);
    root.insert(QStringLiteral("launchopts"), launchopts);       // issue #51 — a new root key; old builds ignore it (mergeAll reads by name)
    root.insert(QStringLiteral("pad2key"), pad2key);             // issue #105 — a new root key; old builds ignore it (mergeAll reads by name)
    root.insert(QStringLiteral("speed"), speed);                 // issue #140 — a new root key; old builds ignore it (mergeAll reads by name)
    root.insert(QStringLiteral("lyricoffset"), lyricoffset);     // issue #142 — a new root key; old builds ignore it (mergeAll reads by name)
    root.insert(QStringLiteral("trackerlink"), trackerlink);     // issue #156 - a new root key; old builds ignore it (mergeAll reads by name)
    root.insert(QStringLiteral("missed"), missed);
    root.insert(QStringLiteral("stats"), stats);
    root.insert(QStringLiteral("playstats"), playstats);
}

void CloudMerge::mergeAll(const QJsonObject& root)
{
    mergeResume(root.value(QStringLiteral("resume")).toObject(),
                root.value(QStringLiteral("resumeTombs")).toArray());
    mergeRecent(root.value(QStringLiteral("recent")).toObject(),
                root.value(QStringLiteral("recentTombs")).toObject());
    mergeMarks(root.value(QStringLiteral("marks")).toObject());
    mergeFavorites(root.value(QStringLiteral("favorites")).toObject());
    mergeBookmarks(root.value(QStringLiteral("bookmarks")).toObject());  // issue #136: per-book reading bookmarks
    mergeAudioBookmarks(root.value(QStringLiteral("audiobookmarks")).toObject()); // issue #140: per-item audio bookmarks
    mergePlaylists(root.value(QStringLiteral("playlists")).toObject());
    mergePresets(root.value(QStringLiteral("presets")).toObject());      // issue #184: saved filter presets
    mergeMetaOverrides(root.value(QStringLiteral("metaoverrides")).toObject());
    mergeLaunchOpts(root.value(QStringLiteral("launchopts")).toObject());   // issue #51
    mergePad2Key(root.value(QStringLiteral("pad2key")).toObject());         // issue #105: per-game pad2key records
    mergeSpeed(root.value(QStringLiteral("speed")).toObject());             // issue #140: per-item speed memory
    mergeLyricOffset(root.value(QStringLiteral("lyricoffset")).toObject()); // issue #142: per-item lyric offset
    mergeTrackerLink(root.value(QStringLiteral("trackerlink")).toObject()); // issue #156: per-item tracker links
    mergeMissed(root.value(QStringLiteral("missed")).toObject());
    const QString localDevice = Settings::deviceId();
    mergeNamespaced(QStringLiteral("stats"),     root.value(QStringLiteral("stats")).toObject(),     localDevice);
    mergeNamespaced(QStringLiteral("playstats"), root.value(QStringLiteral("playstats")).toObject(), localDevice);
    store().sync();
    ItemMarks::invalidate();      // the merge wrote marks/* under the ini directly; drop the stale static cache
    // Ditto for metaoverrides/* (issue #24): its lazy cache would otherwise go on showing the old scrape.
    // Dropping the cache is ALL that happens — no live surface is refreshed, so a peer's correction lands on
    // screen at the next natural rebuild (leaving the level, a Home re-render, the next launch) rather than
    // mid-navigation. Deliberate, and the same behaviour marks have had since they started syncing: the
    // alternative is rebuilding the browse model, and re-issuing the level's request, under the user's
    // cursor on a background merge. The correction is never LOST by waiting — every read composites it.
    MetaOverrides::invalidate();
    LaunchOpts::invalidate();       // ditto for the per-game launch-override cache (issue #51)
    Pad2KeyStore::invalidate();     // ditto for the per-game pad-to-keyboard cache (issue #105)
    MissedDismiss::invalidate();    // ditto for the per-show dismissal cache the "You missed" rule reads
    ConsumptionStats::invalidate(); // ditto for the summed-across-devices stats cache
    Tombstones::compact(30);      // keep the deleted/* footprint bounded (cheap; runs at every merge)
}
