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
#include "../core/TraktRead.h"   // CalendarEntry + imdbStreamIdFor — the Trakt read layer (#23)
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

    // One playlist's contents: PlaylistEntry -> MediaItem. Each entry carries its OWN addonId (playlists are
    // category-scoped and may be mixed-source), stamped onto the row's sourceAddonId so activateItem resolves
    // the right addon per entry — the playlist level itself is addon-less. A "steam:" itemId launches natively
    // (mime "steamgame"); a local-file entry (path set) re-opens by path (mime "localgame:<kind>", url = path).
    MediaCatalog playlistItemsCatalog(const Playlist& p);

    // The single PC Games folder: ONE MediaItem per game, with every way to launch it carried as a source on
    // that item (MediaItem::pcSources). It replaces the four per-launcher folders this file used to build
    // (steamGamesCatalog / epicGamesCatalog / gogGamesCatalog / battleNetGamesCatalog, all deleted with the
    // folders themselves), where the same game appeared up to five times with unrelated ids. Pure, like every
    // builder here: plain lists in, a
    // MediaCatalog out, no UI and no store singleton — in particular it groups with pcgame::itemId (pure)
    // and never consults the user-override ini, which is a store.
    //
    // Per item: id = pcgame::itemId(title) — the SAME function pcgame::remapTable moves records onto, and
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
}
