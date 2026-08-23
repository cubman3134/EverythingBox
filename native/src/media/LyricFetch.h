// The ONLINE half of issue #142's third lyric source: one LRCLIB lookup per track, cached in that track's
// MetaCache folder so it is fetched once and works offline for good. The protocol itself (what URL a set of
// tags produces, what a reply means, which of its two lyric fields wins) is pure and lives in LyricSources.h;
// this file is only the socket and the file.
//
// WHY THE CACHE IS METACACHE AND NOT A NEW STORE. #142 names it: "cache the result in the item's MetaCache
// folder". That folder already exists per item, is already what "this item's offline copy" means everywhere
// else in the app, and is already deleted with the item on uninstall — a private lyrics directory would need
// its own eviction, its own uninstall hook and its own answer to "where did the disk go".
//
// WHY A MISS IS ALSO CACHED, AND WHY IT EXPIRES. Without a recorded miss, every play of a track LRCLIB has
// never heard of is two more requests to a free volunteer-run service, forever. With a permanent one, a track
// whose lyrics somebody contributes next month never gets them, because the app stopped asking. So a miss is
// recorded WITH ITS TIMESTAMP and is honoured for missRetryDays — long enough that a service that does not
// have a track is not asked about it on every listen, short enough that the database being alive still
// reaches the user.
//
// POLITENESS. Nothing here is ever called by a scan. The single caller is the now-playing surface, when a
// track starts, and only after LyricSources::needsOnline() has confirmed that neither local tier answered and
// Settings::onlineLyrics() has confirmed the user wants it. There is no sweep, no prefetch and no queue.
#pragma once
#include "LyricSources.h"
#include <QString>
#include <functional>

namespace LyricFetch
{
    // The MetaCache key for a local audio file: its absolute path. That is the same identity MetaCache::keyFor
    // gives a local MediaItem (no addon id, so the url — which for a local row IS the path), so a track's
    // lyrics land in the same folder as the rest of its cached metadata rather than in a second one beside it.
    QString cacheKey(const QString& audioPath);

    // How long a recorded miss suppresses another lookup. A constant rather than a setting: it trades one
    // volunteer-run service's load against how fast a newly contributed file reaches a listener, and neither
    // side of that is a preference a user can meaningfully hold.
    int missRetryDays();

    // Pure so the expiry rule is assertable without waiting a week. Both arguments are seconds since the
    // epoch; a zero/absent `missAt` is "no miss recorded", which is never fresh.
    bool missIsFresh(qint64 missAtSecs, qint64 nowSecs);

    // The cached answer, or "" when there is none. Reads the file the last successful fetch wrote; no network
    // and no parsing beyond the file read, so it is safe to call on every track change.
    QString cachedText(const QString& key);

    // True when a previous lookup found nothing AND that verdict has not yet expired.
    bool missRecorded(const QString& key);

    // Write an answer / a verdict into the item's folder. Exposed (rather than private to fetch) because the
    // probe pins the round-trip, and because a future "paste your own lyrics" surface would write through the
    // same door instead of inventing a second layout.
    void storeText(const QString& key, const QString& text);
    void storeMiss(const QString& key);

    // The one networked entry point. Calls onDone exactly once, with the lyric text or "" — including
    // synchronously, before returning, when the answer is already cached or the query is unusable, so a caller
    // must not assume it is deferred. Never throws, never blocks the caller's thread on the network.
    void fetch(const QString& key, const Lrclib::Query& query, std::function<void(const QString&)> onDone);
}
