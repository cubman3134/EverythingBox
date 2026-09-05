// THE AUDIOBOOK MATCH RECORD, AND THE REJECTION THAT OUTLIVES EVERY RE-SCAN (issue #198).
//
// One record per matched book: which provider matched it, to what, how sure it was, and the handful of
// scalar fields the browse index is rebuilt from (narrator, series + position, year, runtime). Plus the one
// bit that is not a cache at all — `rejected`.
//
// ---- WHY THIS IS NOT ONLY IN MetaCache ---------------------------------------------------------------------
//
// The heavy half of a match — the cover image and the description — IS in MetaCache, exactly as #73/#134's
// enrichment persists everything else, and for the same payoff: the detail card and the poster then render
// offline through the read path every other surface already uses, with no new code. That half is a CACHE and
// behaves like one. This store holds the other two things, neither of which a cache may hold:
//
//   * THE REJECTION IS INTENT. MetaCache is regenerable by definition — it is excluded from sync, evicted by
//     the image cap, and deleted wholesale by MetaCache::remove(). A rejection that a refresh silently
//     discards is worse than no rejection at all, because the user is never told and the wrong narrator
//     simply comes back. So it lives in the portable everythingbox.ini beside the other per-item intent
//     stores (marks / favourites / metaoverrides / resume), which is also the only place CloudMerge can
//     carry it between devices. MetaOverrides.h makes this argument at length for the same reason; this is
//     the same decision, not a second one.
//
//   * THE INDEX-FACING FIELDS ARE READ FOR EVERY BOOK AT EVERY INSTALL. Re-deriving the browse index after a
//     scan asks "what was matched" once per book — and a MetaCache read is one file open per item. On the
//     GUI thread, for a collection of several hundred books, that is a stall you can see. This store is one
//     ini group, cached on first touch, so the whole answer costs one read per process.
//
// Layout — GLOBAL, not per profile, the same posture MetaOverrides takes and for the same reason: a
// mis-matched book is mis-matched for the whole household.
//   audiobookmatches/items/<md5(bookKey)> -> compact JSON (AudiobookMeta::toJson)
//
// THE KEY IS THE BOOK KEY, i.e. AudiobookLibrary::bookKeyFor / Book::key — folder plus book identity. It is
// hashed with the SAME full MD5-hex-over-UTF8 as ItemMarks and MetaOverrides; no new scheme. It is NOT the
// browse row id (`audiobookbook:<key>`), which is what MetaCache keys through: this store is asked its
// question by the SCAN, which has entries and no rows.
//
// ---- WHAT A REJECTION MEANS ------------------------------------------------------------------------------
//
// The book is not enriched. Not "not by that provider", not "not that edition" — a rejected book stays
// exactly as the scan found it, through this re-scan and every later one, until the user clears it. That is
// blunter than "refuse this one match id" and it is the right blunt: the sweep asks the same providers the
// same question and would hand back the same top hit, so a per-id refusal is a promise to re-fetch, re-score
// and re-reject the same record forever. `reject` therefore keeps the identity of what was refused (so the
// surface can still say what it was) and clears every field, and the sweep skips any book carrying a record
// with the bit set.
//
// A REJECTION IS NEVER SPELLED AS AN ABSENCE, and that is what makes it survive a re-scan: removing the row
// would be indistinguishable from "never matched", and the next sweep would match it again within seconds.
#pragma once
#include "AudiobookMeta.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <functional>

namespace AudiobookMatches
{
    QString hashKey(const QString& bookKey);   // md5-hex of the UTF-8 book key (ItemMarks' scheme)

    // The record for one book; a default (all-empty, not rejected) Match when there is none.
    AudiobookMeta::Match get(const QString& bookKey);
    bool has(const QString& bookKey);          // any record at all, including a rejection
    bool isRejected(const QString& bookKey);

    // Every record for the given books, keyed by BOOK KEY — what the index rebuild applies. One cached ini
    // read however many books are asked about; books with no record are simply absent from the result.
    QHash<QString, AudiobookMeta::Match> forBooks(const QStringList& bookKeys);

    // Store a match. Stamps updatedAt, refuses to overwrite a REJECTION (that is the whole point of the bit),
    // and writes nothing for a record with no fields and no identity.
    void set(const QString& bookKey, const AudiobookMeta::Match& m);

    // "No, that is not this book." Keeps provider/matchId/matchTitle so the surface can still name what was
    // refused; clears every field; sets the bit. Permanent until clear()/clearAll().
    void reject(const QString& bookKey);

    void clear(const QString& bookKey);        // forget the record entirely — the book becomes matchable again
    void clearAll();                           // ...for every book

    int  count();                              // books carrying an APPLIED match
    int  rejectedCount();                      // books the user said no to

    void invalidate();                         // drop the cache (external ini change / after a cloud merge)

    // Multi-device sync trigger, the ItemMarks::setChangeHook contract: fired after every mutation so
    // MainWindow can (re)arm the debounced push. Unset in headless probes (fires nothing).
    void setChangeHook(std::function<void()> hook);
}
