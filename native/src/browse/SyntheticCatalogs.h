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

    // system scopes a games console (SystemCatalog id, or "pc"); empty system matches any. Only local-file
    // favourites (a path set) have a per-console home — streamed favourites are skipped.
    MediaCatalog favoritesCatalog(const QList<FavoriteItem>& all, const QString& system);

    // The FavoriteItem for starring a local game (a Recent/Downloaded/themed list row): identity is the
    // stable key, else the path; re-opens by path. Crucially stamps `system` — favoritesCatalog above only
    // shows favourites whose system matches the console — from the caller's hint (the Recent/Downloads
    // store entry, which knows ambiguous-extension consoles) or, failing that, the ROM extension.
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

    // The Steam console grid, built natively from the local library (no addon request). Each installed SteamGame
    // maps to a MediaItem (id "steam:"+appid, mime "steamgame" — no url, so clicking opens the info page and Play
    // launches it). A non-empty query scopes to games whose name matches (case-insensitive; trimmed). `owned` is
    // the optional creds-gated owned library: any owned game NOT in `installed` is appended, badged "Not
    // installed" (subtitle) with url steam://install/<appid> so activation hands the install to Steam; installed
    // entries are untouched. poster resolves the vertical-capsule artwork; default {} uses SteamLibrary::posterUrl
    // (which touches the local librarycache) — a test injects a pure one to stay I/O-free.
    MediaCatalog steamGamesCatalog(const QList<SteamGame>& installed, const QString& query,
                                   const std::function<QString(const SteamGame&)>& poster = {},
                                   const QList<SteamGame>& owned = {});

    // The Epic console grid, built natively from the local manifests. Each EpicGame -> a MediaItem (id
    // "epic:"+AppName, mime "epicgame" — no url, so clicking opens the info page and Play launches it via the
    // launcher URI, mirroring steamgame). A non-empty query scopes by name (case-insensitive). Epic has no
    // local capsule convention, so poster defaults to empty (title-keyed scrapers fill art later); a test may
    // inject one. Pure: no EpicLibrary I/O here.
    MediaCatalog epicGamesCatalog(const QList<EpicGame>& installed, const QString& query,
                                  const std::function<QString(const EpicGame&)>& poster = {});

    // The GOG console grid, built natively from the registry. Each GogGame -> a MediaItem (id "gog:"+id, mime
    // "goggame", url = the resolved exe — GOG games are DRM-free processes launched through the MONITORED
    // launchPcExe path, so the exe rides on the tile). A non-empty query scopes by name. poster defaults to
    // empty (no local capsule; title-keyed scrapers fill art later).
    MediaCatalog gogGamesCatalog(const QList<GogGame>& installed, const QString& query,
                                 const std::function<QString(const GogGame&)>& poster = {});
}
