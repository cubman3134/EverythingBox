// THE JELLYFIN BROWSE LEVELS, AS PURE BUILDERS (issue #83, on #160's foundation) — the user's libraries,
// one library's titles, a series' seasons, a season's episodes, and the Continue Watching rows the home
// list merges in. Lists in, a MediaCatalog out. No network, no settings, no store, no UI: exactly the rule
// SyntheticCatalogs.h and MusicCatalogs.h are written to, so probe_browse drives every level of this with
// no server and no account.
//
// ==================================================================================================
// WHAT A ROW IS, AND WHY NONE OF THEM CARRIES A URL
// ==================================================================================================
// Every playable row here is stamped with `mime` = "jellyfinitem:" + its QUALIFIED id and with an EMPTY `url`.
// That is not an omission — it is the whole credential design, one level up from Jellyfin.h's section 3:
//
//   * a Jellyfin stream url carries the token in its query, so a url sitting on a browse row would be a
//     credential inside a model that gets copied into playlists, favourites and recents;
//   * the link is minted at the moment the player is handed it (JellyfinClient::playUrlFor / the
//     PlaybackInfo route) and dropped immediately after;
//   * and the id is what survives: the token can rotate, the server can move behind a certificate, and the
//     row still opens, because what was written down names the ITEM rather than one morning's link to it.
//
// This is the same conclusion #203 reached for a Live TV channel, and it is why the leaf is a KEYED local
// leaf kind (browse::LeafPlay::JellyfinItem) rather than an ordinary url row: LeafRoute's OpenFile route
// hands `url` to the player, and there is deliberately nothing there to hand.
//
// ==================================================================================================
// TAGGING A ROW WITH THE SERVER IT CAME FROM
// ==================================================================================================
// #160 asks for rows "tagged with their server name where it disambiguates", and the qualifier is doing
// real work: with ONE server configured the tag is on every row and tells nobody anything, which is how a
// subtitle line that could have shown a year or an episode number gets spent on noise. So the builders take
// `tagServers` and the caller passes "more than one server contributed to this level". Both arms are
// pinned by probe_browse, because a tag that is always on and a tag that is never on are the two ways this
// is wrong and they look identical from one screenshot.
//
// ==================================================================================================
// ORDER, AND A SERVER THAT DID NOT ANSWER
// ==================================================================================================
// Order is whatever the union gave: servers in configured order, items in the order that server listed
// them (Jellyfin.h section 4). Nothing here re-sorts, because a shelf that reshuffles between two refreshes
// for no reason the user can see is its own bug.
//
// A server that timed out or is switched off contributes no rows — and its NOTE is rendered as a trailing
// non-actionable "info" row rather than being dropped, so a half-loaded shelf says so on the screen the
// user is looking at instead of only in a log. An entirely empty level gets a guidance row for the same
// reason: browseItems lets an "info" row through alone precisely so a themed column is never simply blank.
#pragma once
#include "../addons/AddonModels.h"
#include "../core/Jellyfin.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace browse
{
    // ---- The row kinds ---------------------------------------------------------------------------------
    // Synthetic ('_'-prefixed) types DRILL on both layouts (LeafRoute::themedEnterFor sends them down the
    // ordinary browse path); the leaf below is the only one that opens the themed inline chooser.
    inline const char* kJellyfinRootType    = "_jellyfin";      // the Video-root folder row: "Jellyfin"
    inline const char* kJellyfinRootPrefix  = "jellyfin:";      // ...and the marker loadTop() repopulates from
    inline const char* kJellyfinLibType     = "_jflibrary";
    inline const char* kJellyfinLibPrefix   = "jflibrary:";     // + <qualified library id>
    inline const char* kJellyfinSeriesType  = "_jfseries";
    inline const char* kJellyfinSeriesPrefix = "jfseries:";     // + <qualified series id>
    inline const char* kJellyfinSeasonType  = "_jfseason";
    // + <qualified series id> "\n" <qualified season id>. TWO ids, because /Shows/<seriesId>/Episodes is
    // addressed by the SERIES and filtered by the season — the OPDS feed level's own two-part marker
    // ("opdsfeedlvl:<catalogId>\n<feedUrl>") for the same reason, and a '\n' rather than a delimiter that
    // could occur inside an id.
    inline const char* kJellyfinSeasonPrefix = "jfseason:";

    // --- LOCAL LEAF KIND (declared with the feature that stamps it — see LeafRoute.h) ---
    // A PLAYABLE JELLYFIN ITEM. Keyed: the mime is this prefix followed by the row's qualified id, and
    // jellyfinKeyOf reads it back. It is a "local" leaf in LeafRoute's sense — no addon can resolve it —
    // even though the bytes are on a server: what the word means there is "this app owns the route".
    inline const char* kJellyfinItemPrefix = "jellyfinitem:";

    // The key a keyed mime carries: EVERYTHING after the prefix, never a section(':') — a qualified id is
    // itself full of colons, so a split would truncate every one of them into a different item's id. The
    // same rule (and the same reason) as browse::musicKeyOf.
    inline QString jellyfinKeyOf(const QString& mime, const char* prefix)
    {
        const QString p = QString::fromLatin1(prefix);
        return mime.startsWith(p) ? mime.mid(p.size()) : QString();
    }

    // ---- The levels ------------------------------------------------------------------------------------

    // One row per library, across every server that answered. `tagServers` puts the server's display name
    // on each row's second line (see the header). `notes` are Jellyfin::unavailableNote strings — one per
    // server that contributed nothing — and are appended as non-actionable rows.
    // Jellyfin::LibraryRef, and NOT a struct of this file's own: the union that produces these rows is in
    // Jellyfin.cpp, and a second copy of the same four fields is how a builder and its producer drift.
    MediaCatalog jellyfinLibrariesCatalog(const QVector<Jellyfin::LibraryRef>& libraries,
                                          const QStringList& notes);

    // The titles inside one library: a Movie is a LEAF, a Series is a CONTAINER that drills into its
    // seasons. `title` is the level's own heading (the library's name).
    MediaCatalog jellyfinLibraryCatalog(const QString& title,
                                        const QVector<Jellyfin::UnionItem>& items,
                                        bool tagServers, const QStringList& notes);

    // A series' seasons. Each drills into that season's episodes, carrying BOTH ids (see kJellyfinSeasonPrefix).
    MediaCatalog jellyfinSeasonsCatalog(const QString& seriesTitle, const QString& seriesRef,
                                        const QVector<Jellyfin::UnionItem>& seasons);

    // A season's episodes, as leaves. Numbered "S1E4 · Title" when the server gave numbers, and by title
    // alone when it did not — a special or an extra genuinely has no number, and "S0E0" in front of it
    // would be this app inventing one.
    MediaCatalog jellyfinEpisodesCatalog(const QString& seasonTitle,
                                         const QVector<Jellyfin::UnionItem>& episodes);

    // The server's own Continue Watching, as rows for the home list's Recents surface — merged in beside
    // the local ones the way the Trakt sections already are. Each row is a leaf, identical in shape to the
    // one the browse levels build, so pressing it takes exactly the same route.
    QVector<MediaItem> jellyfinContinueRows(const QVector<Jellyfin::UnionItem>& items, bool tagServers);

    // The ONE row builder every level above shares. Exposed because probe_browse asserts the shape once
    // rather than four times, and because a second copy of "what a playable Jellyfin row looks like" is
    // exactly how the url would come back.
    MediaItem jellyfinLeafRow(const Jellyfin::UnionItem& it, bool tagServer);
}
