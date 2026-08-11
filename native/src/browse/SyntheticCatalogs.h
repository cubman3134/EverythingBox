// Pure builders for the synthetic browse-catalog levels (Recent / Downloaded / Favorites): the folders that
// show up under a catalogue or games console but aren't backed by an addon — they're built from the local
// RecentStore/DownloadsStore/FavoritesStore lists. Extracted out of HomeView so the filtering/mapping rules
// (kind+system scoping, the pcgame-counts-as-game rule, offline-first artwork, missing-file hiding) are
// testable without a live app: each function takes the store's list as a plain argument and returns a
// MediaCatalog, with no HomeView/UI/store-singleton dependency.
#pragma once
#include "../addons/AddonModels.h"
#include "../core/RecentStore.h"
#include "../core/DownloadsStore.h"
#include "../core/FavoritesStore.h"
#include "../core/PlaylistStore.h"
#include "../core/SteamLibrary.h"
#include "../core/EpicLibrary.h"
#include "../core/GogLibrary.h"
#include "../core/BattleNetLibrary.h"
#include "../core/LocalLibrary.h"
#include "../core/PcGameId.h"     // pcGamesCatalog: the merge key + PcGameSource
#include "../core/IptvSourceStore.h" // liveTvSourcesCatalog: the saved IptvSource (#75 inc 2)
#include "../media/StreamResolver.h" // liveTvChannelsCatalog: the parsed M3uEntry (#75)
#include "../core/TraktRead.h"   // CalendarEntry + imdbStreamIdFor — the Trakt read layer (#23)
#include "../core/TraktSync.h"   // TraktListEntry — the watchlist/collection rows (#23)
#include "../core/TraktMissed.h" // MissedRow — the "You missed" selection rule's output (#25)
#include <QHash>                  // liveTvChannelsCatalog: the now/next-by-tvg-id subtitle map (#75 inc 3)
#include <functional>

namespace browse
{
    // Map a Recent/Downloaded entry's kind to a media type (for the placeholder icon) and a human label.
    QString iconTypeForKind(const QString& kind);

    // marker = "<kind>" or "<kind>|<system>": the optional system scopes a games console (its SystemCatalog id,
    // or "pc"); empty system = all of that kind (the catalogue-root Recent). PC games count as "game".
    MediaCatalog recentsCatalog(const QList<RecentItem>& all, const QString& marker);

    // marker = "<kind>|<system>": kind filters the catalogue; system (a SystemCatalog id, or "pc") scopes a
    // games console. An empty system matches any (non-game catalogues). fileExists lets a test inject a fake
    // existence check; default {} uses QFileInfo::exists.
    MediaCatalog downloadsCatalog(const QList<DownloadedItem>& all, const QString& marker,
                                  const std::function<bool(const QString&)>& fileExists = {});

    // Build a "Local Library" catalog from scanned local video entries. Each becomes a playable
    // MediaItem (url = local path, mime = "local:video"); id = imdb id when known, else "local:<path>".
    MediaCatalog localLibraryCatalog(const QVector<LocalLibrary::VideoEntry>& entries);

    // system scopes a games console (SystemCatalog id, or "pc"); empty system matches any. Only local-file
    // favourites (a path set) have a per-console home — streamed favourites are skipped.
    //
    // ONE EXCEPTION: a MERGED PC game ("pcgame:<key>"). It deliberately has no path — which copy runs is
    // decided at activation by the source picker, so there is no single file to record — and the path test
    // alone would drop every starred PC game out of the PC console's ★ Favorites folder while still showing
    // it on Home, which reads as the star having silently failed. Its id names the game, so it is kept and
    // re-opened by re-deriving its sources.
    MediaCatalog favoritesCatalog(const QList<FavoriteItem>& all, const QString& system);

    // The FavoriteItem for starring a local game (a Recent/Downloaded/themed list row): identity is the
    // stable key, else the path; re-opens by path. Crucially stamps `system` — favoritesCatalog above only
    // shows favourites whose system matches the console — from the caller's hint (the Recent/Downloads
    // store entry, which knows ambiguous-extension consoles) or, failing that, the ROM extension. A merged
    // PC game has neither (no path to derive from, and it is in no Recent/Downloads store), so its "pcgame:"
    // id stamps `system` = "pc" directly.
    FavoriteItem localGameFavorite(const MediaItem& it, const QString& systemHint);

    // The Playlists folder for one CATEGORY: a row per playlist (drills into playlistItemsCatalog) followed by
    // the trailing synthetic "_newplaylist" row (activation opens the name prompt). categoryKey rides that New
    // row's mime so activation creates in the right bucket. Pure: addon resolution happens later, per-entry, at
    // activation time — so no addon data is needed here.
    MediaCatalog playlistsCatalog(const QList<Playlist>& all, const QString& categoryKey);

    // ---- Live TV (#75, increment 2) ----------------------------------------------------------------------
    // The "Live TV" folder: one row per SAVED IptvSource (drilling into its channels), plus a trailing
    // synthetic "add a source" row — the way playlistsCatalog appends its "_newplaylist" row. An empty source
    // list yields JUST the add row, so a user with no sources still has the primary "add a source" path. Pure:
    // the channels are fetched later, per source, at activation time — no network here.
    //   source row: type "_livetvsource", mime "livetvsource:<id>"   (activation fetches + shows its channels)
    //   add row:    type "_newlivetv",    mime "newlivetv"           (activation opens the name/URL prompt)
    MediaCatalog liveTvSourcesCatalog(const QList<IptvSource>& sources);

    // The channels of ONE source, SECTIONED by group-title (reusing increment 1's grouping): the groups are
    // ordered case-insensitively with the empty-group "Ungrouped" bucket LAST, and each group is introduced by a
    // non-activatable header row (type "_livetvheader") followed by that group's channels in playlist order. Each
    // channel is a playable MediaItem carrying the stream url and the tvg-logo as tile art; a channel whose id is
    // in `favs` (a FavoriteItem with type "livetv") is marked with a leading ★ on its title. Pure: parsed
    // entries in -> catalog out, no network, no store read.
    // `nowNextByTvgId` (#75 inc 3, default empty): tvg-id -> a now/next one-liner ("Now: X · Next: Y"). When a
    // channel's tvg-id has an entry, that string REPLACES the group as the channel's subtitle (its second line);
    // channels with no EPG match keep the group, exactly the increment-2 behaviour. The map is a plain
    // QHash<QString,QString> — deliberately NOT the XMLTV type — so this builder stays free of any EPG
    // dependency (the caller computes the strings; see browse::liveTvNowNextByTvgId).
    MediaCatalog liveTvChannelsCatalog(const QString& sourceName, const QVector<M3uEntry>& entries,
                                       const QList<FavoriteItem>& favs,
                                       const QHash<QString, QString>& nowNextByTvgId = {});

    // A channel's stable identity — the key its favourite is stored under and re-opened by. The stream url, which
    // is what re-plays it; built in ONE place so the catalog's mark and the toggle's write can never disagree.
    QString liveTvChannelId(const M3uEntry& e);

    // The FavoriteItem for starring a Live TV channel: type "livetv", the stream url in BOTH itemId (via
    // liveTvChannelId) and path, so isFavorite() marks it and re-opening plays it. Mirrors localGameFavorite.
    FavoriteItem liveTvChannelFavorite(const M3uEntry& e);

    // One playlist's contents: PlaylistEntry -> MediaItem. Each entry carries its OWN addonId (playlists are
    // category-scoped and may be mixed-source), stamped onto the row's sourceAddonId so activateItem resolves
    // the right addon per entry — the playlist level itself is addon-less. A "steam:" itemId launches natively
    // (mime "steamgame"); a local-file entry (path set) re-opens by path (mime "localgame:<kind>", url = path).
    MediaCatalog playlistItemsCatalog(const Playlist& p);

    // The single PC Games folder: ONE MediaItem per game, with every way to launch it carried as a source on
    // that item (MediaItem::pcSources). It replaces the four per-launcher folders this file used to build
    // (steamGamesCatalog / epicGamesCatalog / gogGamesCatalog / battleNetGamesCatalog, all deleted with the
    // folders themselves), where the same game appeared up to five times with unrelated ids. Plain lists in, a
    // MediaCatalog out, and no UI.
    //
    // ONE store read, and it is deliberate: grouping goes through pcgame::effectiveItemId, which consults the
    // user's merge-override ini. That is the escape hatch the design named as the thing that makes a fuzzy
    // title heuristic shippable, and it has to be spent at the point identity is minted or the catalog and the
    // record remap end up keying on different ids. With no verdict recorded the read changes nothing —
    // effectiveItemId is then exactly itemId — so a probe with a clean data dir sees the pure builder it saw
    // before. (Every probe_* target compiles with EB_ISOLATED_DATA_DIR and therefore starts with an empty ini,
    // so a probe that wants the override branch writes a verdict itself; see probe_browse §pcgames-override.)
    //
    // Per item: id = pcgame::effectiveItemId(title) — the SAME function pcgame::remapTable moves records onto, and
    // the only place that id is built. The two used to compute it separately and could disagree, which
    // silently strands the user's favourites, marks and play time under a key nothing reads; probe_browse
    // now pins them equal. mime = "pcgame", the ONE routing kind replacing steamgame /
    // epicgame / goggame / battlenetgame; url EMPTY, because which copy runs is decided at activation.
    //
    // `downloaded` is the already-built source list for locally downloaded copies (PcGameStore). Its `label`
    // doubles as that copy's TITLE — it is the only human-readable field on a PcGameSource — and is kept
    // verbatim for the picker row. A source with no title to group on is skipped rather than bucketed with
    // every other nameless one.
    //
    // DISPLAY TITLE: a launcher's own name beats a file-provider release name (which carries scene tokens),
    // by the fixed precedence steam > epic > gog > battlenet > downloaded. That precedence keys on the
    // source's KIND first and only then on its `launcher`, so a Downloaded source loses whatever `launcher`
    // it happens to carry — the rule is in the code, not in an assumption that the field is empty. Two
    // titles at the same rank are
    // settled by comparing them, which picks the base title over its edition variant (the edition suffix
    // sorts after the bare name). Fixed, so the folder does not reshuffle between runs.
    //
    // SOURCE ORDER is ready-before-not-ready, then by launcher name (a downloaded/addon source has an empty
    // launcher and so leads the ready rows), then launchId, then label — deterministic, so the picker's rows
    // are stable and a probe can assert them.
    //
    // A Battle.net title with no product code has no protocol launch: it gets a source LABELLED as the
    // best-effort exe it is, and — when even that exe is unknown — a not-ready one, so pickAutoSource can
    // never hand Play something that does nothing.
    //
    // SAME-LAUNCHER ROWS ARE DISAMBIGUATED. The merge key is lossy by design (the trailing-year strip fuses
    // "Prey (2006)" with "Prey (2017)"), so one group can hold two sources from the SAME launcher: both
    // labelled "Steam", both ready, both showing the bare "Prey". The launches are fine — the launchIds
    // differ — but the picker rows read identically. A launcher that contributed TWO OR MORE sources gets
    // its own per-launcher title appended to each of its labels ("Steam · Prey (2017)"); a launcher that
    // contributed one is left with its plain label. Should two such copies share a title as well, the
    // launchId is appended as a backstop.
    //
    // `query` filters on the NORMALISED title (any of the game's contributing titles, not just the displayed
    // one); a query that normalises to nothing ("!!!") falls back to a plain case-insensitive match rather
    // than matching everything. `launcherFilter` ("steam" | "epic" | "gog" | "battlenet") keeps only games
    // that HAVE such a source — it narrows which games appear, not which sources they carry, so "what I own
    // on Steam" survives without a separate folder and still launches by whichever copy is ready. It matches
    // a LAUNCHER source only: a downloaded copy that records which launcher it came from does not make the
    // game appear under "what I own on Steam".
    //
    // poster resolves the tile art from a game's sources; default {} uses SteamLibrary::posterUrl for the
    // Steam source (which touches the local librarycache) and nothing otherwise — a test injects a pure one
    // to stay I/O-free.
    //
    // `steamOwned` is the creds-gated Steam owned library (a Web API key + SteamID; empty without them). Any
    // owned game NOT already installed on Steam contributes a LauncherOwned source: NOT ready, carrying
    // steam://install/<appid>, so pickAutoSource can never launch it from a single Play keypress but choosing
    // its picker row hands the install to the Steam client. It is the ONLY producer of a non-ready launcher
    // source that can still be acted on, and it is why `LauncherOwned` exists — the folder that replaced the
    // Steam console had to keep the owned-but-not-installed library that console showed. It rides last, after
    // `poster`, for the same reason steamGamesCatalog's `owned` did: every existing call site keeps compiling.
    // A game whose sources are ALL not-installed (owned/addon-available) is badged "Not installed" in its
    // subtitle, which is where the old console put that badge.
    MediaCatalog pcGamesCatalog(const QList<SteamGame>& steam, const QList<EpicGame>& epic,
                                const QList<GogGame>& gog, const QList<BattleNetGame>& bnet,
                                const QVector<pcgame::PcGameSource>& downloaded,
                                const QString& query, const QString& launcherFilter,
                                const std::function<QString(const QVector<pcgame::PcGameSource>&)>& poster = {},
                                const QList<SteamGame>& steamOwned = {});

    // ---- The PC Games folder's launcher filter (issue #44) ---------------------------------------------
    // pcGamesCatalog has always taken a `launcherFilter` and it has always worked; every call site passed an
    // empty string and no surface offered it, so "show me what I own on Steam" — which the design used to
    // justify deleting the four per-launcher folders — had no replacement at all. These three build the
    // control that reaches it. Pure: they decide what the folder OFFERS, never what it shows.

    // The launcher's own name, for a row a person reads. Returns an empty string for an id with no name
    // here, which callers use as "not a launcher we can offer".
    QString pcLauncherLabel(const QString& launcher);

    // Which launchers this library actually has games in, in the folder's fixed display order
    // (steam, epic, gog, battlenet). Offering a launcher with nothing behind it is a menu row that can only
    // ever empty the folder, and offering ALL FOUR always would do exactly that on the common machine with
    // one store installed. Owned-but-not-installed Steam entries count: they are Steam library entries, and
    // "what I own on Steam" is the phrase this feature exists to answer.
    QStringList pcLaunchersPresent(const QList<SteamGame>& steam, const QList<EpicGame>& epic,
                                   const QList<GogGame>& gog, const QList<BattleNetGame>& bnet,
                                   const QList<SteamGame>& steamOwned = {});

    // The filter menu: .first is the launcherFilter value to pass to pcGamesCatalog (EMPTY = every
    // launcher), .second is the row a person reads, with the current choice ticked.
    //
    // ONE list of pairs rather than two parallel lists on purpose: the row the user pressed and the value it
    // means have to stay index-aligned, and two lists that must agree by convention is the shape this
    // codebase has already been bitten by (see pcgame::itemId's header).
    //
    // "All launchers" is ALWAYS first, so a filter that has emptied the folder can still be cleared — the
    // rows below it may all have vanished with the library they described.
    QVector<QPair<QString, QString>> pcLauncherFilterChoices(const QStringList& available,
                                                             const QString& current);

    // The control row itself, pinned at the TOP of the PC Games folder. A row in the folder rather than a
    // widget above it because this app has four layouts (classic grid, themed grid, XMB, carousel) and only
    // the classic one has chrome a QComboBox could live in — a filter bar there is a control most users
    // cannot see, and one only a mouse can reach, which is the defect issue #40 is already open about.
    // A folder row is D-pad reachable in every layout by construction.
    MediaItem pcLauncherFilterRow(const QString& current);

    // Episodes airing soon, from a connected Trakt account. Sorted by air time, soonest first.
    // PAST entries are excluded: recently-aired episodes are issue #25's job ("You missed"), and two
    // surfaces both claiming the same episode is worse than either alone. The boundary is CLOSED on the
    // past side — airsAtUtc <= nowUtc is excluded, so an episode airing exactly now belongs to #25, not
    // here. probe_browse pins that exact tick so a later change to it cannot pass silently.
    //
    // An entry whose show has no IMDB id is INCLUDED but left with no imdbStreamId and no url — the
    // app's existing representation of "nothing to play" — and says so in its subtitle. Omitting it
    // would be worse: the user would silently lose a third of their calendar with no way to tell.
    //
    // nowUtc is a PARAMETER, not QDateTime::currentDateTimeUtc(): the exclusion boundary is the one rule
    // here worth pinning, and a builder that read the clock itself could only be tested by waiting.
    // Everything on a CalendarEntry is UTC (see CalendarEntry::airsAtUtc), including the day printed in
    // the subtitle, so pass a UTC `nowUtc` — a local-clock one compares two different clocks and slips
    // the boundary by the offset.
    MediaCatalog traktCalendarCatalog(const QVector<CalendarEntry>& entries, const QDateTime& nowUtc);

    // A Trakt WATCHLIST or COLLECTION folder — the same builder for both, because they are the same
    // list of titles differing only in what putting something on it meant. `title` is the folder's own
    // name, so the caller names it and the shape is stated once.
    //
    // ORDER: most recently added first, which is what a list you keep adding to is for. Ties break on
    // title and then on id, so the folder is a total order and cannot reshuffle between runs.
    //
    // A MOVIE row is playable through the IMDB stream bridge exactly as a calendar row is, and carries
    // "" — the documented "not playable" signal — when Trakt has no usable IMDB id for it.
    //
    // A SHOW row deliberately carries NO imdbStreamId, EVEN WHEN Trakt gave a perfectly good show id.
    // That is the one judgement call in this builder and it is not an omission. The app's stream bridge
    // resolves "tt123" (a movie) and "ttShow:season:episode" (an episode); a bare show id sent as a
    // series resolves to `ep:tt123` for the file provider and /stream/series/tt123.json for Stremio,
    // neither of which can ever match, so the row would look playable, be pressed, spin, and fail. That
    // is precisely the outcome imdbStreamIdFor's guard exists to prevent, and a watchlist is mostly
    // shows — it would be the common case, not an edge. The surface routes a show row into the app's
    // own cross-addon search for its title instead, which reaches the real season/episode browser the
    // user is actually after. Its `mime` marker is what tells the surface which of the two a row is.
    MediaCatalog traktListCatalog(const QVector<TraktListEntry>& entries, const QString& title);

    // "Would traktListCatalog produce any row at all?" — the SAME admissibility rule, short-circuiting
    // on the first row that passes, and building nothing.
    //
    // It exists because the folder list is rebuilt on every navigation into the video root and each of
    // the two Trakt folders was answering that question by constructing and FULLY SORTING its whole
    // catalog, then asking whether the result was empty. A calendar is ~100 rows and a watchlist can be
    // thousands, so a user with a large Trakt library paid an O(n log n) sort of it, twice, to decide
    // whether to draw two folder rows. The rule itself is deliberately NOT restated here — the shared
    // helper both functions call is what keeps this answer and the catalog's row count in agreement,
    // and probe_browse asserts the equivalence over a table rather than trusting it.
    bool traktListHasRows(const QVector<TraktListEntry>& entries);

    // The exact `mime` markers traktListCatalog stamps. They ARE the routing contract — activation on
    // both surfaces keys on them and on nothing else — so they are named here rather than spelled out
    // as literals in HomeView, where a typo would silently fall through to the generic addon path.
    inline const char* kTraktListMovieMime = "trakt:list:movie";
    inline const char* kTraktListShowMime  = "trakt:list:show";

    // ---- "You missed" (issue #25) ------------------------------------------------------------------
    // The rendering half. The SELECTION is trakt::planMissed (TraktMissed.h) and stays there: it is a
    // join over the marks store and the dismissal store, both of which reach it through callbacks, and
    // keeping it out of this file is what lets probe_trakt pin every clause of it with no catalog
    // anywhere near. This turns the rows it produced into tiles and decides nothing else.
    //
    // `maxRows` <= 0 means uncapped, which is what the FOLDER passes. The HOME SHELF passes
    // trakt::kMissedShelfMax: a shelf is a strip you scan on the way past, and one long enough to need
    // scrolling has stopped being a glance. The cap is applied AFTER the rule has ordered the rows, so
    // the eight you see are the eight most recent — never an arbitrary eight.
    //
    // Every row is playable by construction: planMissed drops an episode it cannot key, so unlike the
    // calendar and the watchlist there is no "No source" case to render here and no unplayable tile.
    MediaCatalog traktMissedCatalog(const QVector<trakt::MissedRow>& rows, int maxRows);

    // The `mime` marker a "You missed" row carries, and the two readers for it. It is not a bare
    // constant like the pair above because the row has to carry TWO things activation needs and a
    // MediaItem has nowhere else to put them: WHICH SHOW to file a dismissal under, and THROUGH WHAT
    // TIME to file it. The second is not derivable at the press — the row shows its OLDEST episode,
    // while a dismissal must cover its NEWEST — so recomputing it in the surface would silently dismiss
    // only part of what the row was speaking for and hand the rest straight back.
    //
    // Format: "trakt:missed:<showKey>:<latestAiredUnix>". The show key is an IMDB id that has already
    // been proved to carry no ':' of its own (trakt::missedShowKey applies the same usability test the
    // stream ids do), which is what makes a colon-separated marker safe here. Built and read in ONE
    // place so the two cannot drift; probe_browse pins the round trip.
    QString traktMissedMarker(const QString& showKey, qint64 latestAiredUnix);
    QString traktMissedShowKeyOf(const QString& mime);   // "" when `mime` is not one of these markers
    qint64  traktMissedThroughOf(const QString& mime);   // 0 when it is not, or carries no usable stamp
    bool    isTraktMissedMime(const QString& mime);
}
