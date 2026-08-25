// Which console the BASE-ROM crawl goes looking on — as a pure decision, so the console a romhack's base ROM
// is searched for on is the console the Romhacks verb was OFFERED on.
//
// WHY THIS FILE EXISTS. Pressing Romhacks on a game does two separate things: it asks for that game's hacks
// (by SYSTEM ID), and, when this machine has no copy of the game yet, it fetches the base ROM through the
// ordinary download crawl (by CONSOLE NAME — the crawl resolves a game by searching "<title> <console>").
// Those two halves used to read different signals:
//
//   * the verb is OFFERED when a system resolves from ANY of three — the item's own systemHint, the ROM's
//     parent folder or extension, or the console page it was reached from;
//   * the crawl only ever got the third, by walking the browse stack for a level of type "platform".
//
// So a game reached from Recents, from a search result or from a favourites row — where the hint is the only
// signal there is — offered the verb and then crawled with no console at all: the walk found no platform
// level and left `parentTitle` as whatever the last level happened to be ("Recently played"). The search that
// finds a base ROM cannot answer a bare title; it returns nothing and asks for a console to be named too. The
// whole flow then ended in "Nothing here could be downloaded." after a long wait, with the romhack half of it
// working perfectly — which is exactly why it read as a romhack fault and not a download one.
//
// romhackCrawlParent() is the one answer to "which console does this crawl carry". The caller passes the
// system the verb was offered on, and when the stack has no platform level the console is derived from that
// system id rather than left as the wrong level. A stack that HAS a platform level is untouched: the page you
// drilled in from is the more specific answer, and it stays authoritative.
//
// NOTHING HERE TOUCHES THE WORLD. No UI, no browse stack, no filesystem — the same rule LeafRoute.h and
// MusicCatalogs.h are written to, so probe_romhack can construct the no-platform-level case directly instead
// of needing a window and a navigation history to reproduce it.
#pragma once
#include "../core/SystemCatalog.h"

#include <QString>
#include <QVector>

namespace browse
{
    // One browse-stack level, reduced to what this decision reads. Deliberately NOT a MediaItem: the rule
    // turns on two strings, and taking the whole row would invite it to grow a dependency on the rest.
    // Ordered outermost-first, as the stack itself is (the last element is the level being looked at).
    struct CrawlLevel
    {
        QString title;
        QString type;
    };

    // What a crawl node's parentTitle / parentType should be for a base-ROM download.
    struct CrawlParent
    {
        QString title;
        QString type;
    };

    // `systemId` is the system the Romhacks verb was offered on (retroSystemFor's answer) — empty only if a
    // caller has none, in which case nothing is invented and the old shape stands.
    inline CrawlParent romhackCrawlParent(const QVector<CrawlLevel>& stack, const QString& systemId)
    {
        CrawlParent out;
        if (!stack.isEmpty()) { out.title = stack.last().title; out.type = stack.last().type; }
        // The nearest platform level, exactly as the download crawl has always found it.
        for (int i = stack.size() - 1; i >= 0; --i)
            if (stack.at(i).type == QStringLiteral("platform"))
            {
                out.title = stack.at(i).title;
                out.type = QStringLiteral("platform");
                return out;
            }
        // No platform level: nothing on the stack knows the console, so the SYSTEM the verb was offered on is
        // asked for its name. Reached only when the walk above found nothing, so a console page is never
        // second-guessed — this is the case where the alternative is a crawl that names no console at all and
        // is answered with nothing. An id the catalog does not know yields no name, and then the node keeps
        // the old shape: a console invented out of a guess would search the WRONG platform, which is worse.
        const QString console = SystemCatalog::consoleNameFor(systemId);
        if (!console.isEmpty()) { out.title = console; out.type = QStringLiteral("platform"); }
        return out;
    }
}
