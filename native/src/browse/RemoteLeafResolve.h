// What the download crawl does with a REMOTE source's answer for one leaf — as a pure sequence, so that
// "resolve it by id, and when that finds nothing look it up by title" is one readable rule instead of two
// callbacks that only meet at runtime.
//
// WHY THIS FILE EXISTS. HomeView::dlResolveLeaf has two ways to turn a leaf into a file:
//
//   * a LOCAL script addon is bridged by SEARCHING a file provider — for a game, "<title> <console>";
//   * a REMOTE (http) source is asked for the leaf's own /stream, by ID.
//
// The id path is the right one and stays the fast one, but it only works when the id is one the ROM source
// knows. A game leaf browsed from a console page or a metadata catalog carries a METADATA id (an igdb: or
// tgdb: one), and asking a ROM source to resolve that returns zero streams — correctly, since it has never
// heard of it. The remote path then emitted nothing and moved on, and the crawl ended in "Nothing here could
// be downloaded." The search that WOULD have found it is the one the local path already does, and it was
// simply unreachable from this transport.
//
// So: when the direct resolve comes back empty and the leaf is a game, fall back to the same title+console
// search. This is NOT romhack-specific — it is the ordinary Download verb on any game leaf that came from a
// remote addon; fetching a romhack's base ROM is just the flow that made it visible.
//
// The single-attempt rule matters more than it looks. `finish` is the crawl's dlNext(): calling it twice
// corrupts the queue by advancing past a node nobody resolved, and not calling it at all hangs the crawl on a
// "Preparing download…" toast that never clears. Both failure branches below end in exactly one `finish`, and
// they are written in one place so that reading them is enough to see it.
//
// NOTHING HERE TOUCHES THE WORLD. No network, no UI, no browse stack — the caller's sinks do that. That is
// what lets probe_romhack drive both stages with recording sinks and count the finishes, instead of needing a
// window, a server and a metadata id to reproduce the defect.
#pragma once

#include <QString>
#include <functional>

namespace browse
{
    // The title+console lookup to issue when a remote source could not resolve the leaf by id. Shaped as the
    // local bridge shapes it, because it is answered by exactly that search (AddonManager::resolveDocumentByQuery).
    struct RemoteLeafPlan
    {
        bool    search = false;   // false => there is nothing worth asking; the crawl just moves on
        QString query;            // the words that FIND it: title plus console
        QString wantTitle;        // the title a result is JUDGED by, without the words that only help find one
        QString catalogType;      // the shelf to search
        bool    consoleKnown = false;   // false => a title-only search, which at least one source answers with nothing
    };

    // `directUrl` is what the source's own /stream returned — non-empty means the id resolved and NO search is
    // wanted. That guard is the whole difference between "rescue the ids that cannot resolve" and "double every
    // download's round trips", so it lives in the decision rather than in a caller that could forget it.
    inline RemoteLeafPlan remoteLeafFallbackPlan(const QString& directUrl,
                                                 const QString& itemType, const QString& itemTitle,
                                                 const QString& parentTitle, const QString& parentType)
    {
        RemoteLeafPlan plan;
        if (!directUrl.isEmpty()) return plan;                       // the id resolved: fast path, untouched
        if (itemType != QStringLiteral("game")) return plan;         // only a game is found by title+console
        const QString title = itemTitle.trimmed();
        if (title.isEmpty()) return plan;                            // nothing to search for
        // The console, read exactly as the local bridge reads it: the platform level this leaf sits under.
        const QString console = (parentType == QStringLiteral("platform")) ? parentTitle.trimmed() : QString();
        plan.search       = true;
        plan.consoleKnown = !console.isEmpty();
        plan.query        = (title + QLatin1Char(' ') + console).trimmed();
        // Judged by its own title alone. The console is a word that helps FIND the ROM; leaving it in here
        // would make every candidate look wrong, and a title-mismatched result is REFUSED — a wrong ROM is
        // worse than none, because the patch is then applied to the wrong dump and only fails much later.
        plan.wantTitle    = title;
        plan.catalogType  = QStringLiteral("game");
        return plan;
    }

    // What the crawl does with a leaf, once and only once. `emitFound` queues the file; `search` issues the
    // plan (its answer comes back through remoteLeafSearchDone); `finish` is dlNext().
    struct RemoteLeafSinks
    {
        std::function<void(const QString& url, const QString& mime)> emitFound;
        std::function<void(const RemoteLeafPlan&)>                   search;
        std::function<void()>                                        finish;
    };

    // Stage 1: the source answered the direct /stream.
    inline void remoteLeafResolved(const QString& directUrl, const QString& directMime,
                                   const QString& itemType, const QString& itemTitle,
                                   const QString& parentTitle, const QString& parentType,
                                   const RemoteLeafSinks& sinks)
    {
        const RemoteLeafPlan plan = remoteLeafFallbackPlan(directUrl, itemType, itemTitle, parentTitle, parentType);
        if (plan.search) { sinks.search(plan); return; }   // finish comes from stage 2, not from here
        if (!directUrl.isEmpty()) sinks.emitFound(directUrl, directMime);
        sinks.finish();
    }

    // Stage 2: the title+console search answered. The only path that reaches here is the one that issued a
    // search above, so this `finish` is the same single one that stage 1 deferred.
    inline void remoteLeafSearchDone(const QString& url, const QString& mime, const RemoteLeafSinks& sinks)
    {
        if (!url.isEmpty()) sinks.emitFound(url, mime);
        sinks.finish();
    }
}
