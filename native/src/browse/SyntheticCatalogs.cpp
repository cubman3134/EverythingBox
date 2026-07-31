#include "SyntheticCatalogs.h"
#include "../core/MetaCache.h"
#include <QFileInfo>
#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <algorithm>

namespace browse
{

QString iconTypeForKind(const QString& kind)
{
    if (kind == QStringLiteral("video"))    return QStringLiteral("movie");
    if (kind == QStringLiteral("audio"))    return QStringLiteral("album");
    if (kind == QStringLiteral("document")) return QStringLiteral("book");
    if (kind == QStringLiteral("game") || kind == QStringLiteral("pcgame")
        || kind == QStringLiteral("steamgame") || kind == QStringLiteral("epicgame")
        || kind == QStringLiteral("goggame")
        || kind == QStringLiteral("battlenetgame")) return QStringLiteral("game");
    return QString();
}

MediaCatalog recentsCatalog(const QList<RecentItem>& all, const QString& marker)
{
    // marker = "<kind>" or "<kind>|<system>": the optional system scopes a games console (its SystemCatalog id,
    // or "pc"); empty system = all of that kind (the catalogue-root Recent).
    const QString kind = marker.section(QLatin1Char('|'), 0, 0);
    const QString system = marker.section(QLatin1Char('|'), 1, 1);
    MediaCatalog cat; cat.title = QObject::tr("Recent");
    for (const RecentItem& r : all)
    {
        // PC + Steam games belong to the game catalogue's Recent view alongside emulated ones.
        const bool match = r.kind == kind
                           || (kind == QStringLiteral("game")
                               && (r.kind == QStringLiteral("pcgame") || r.kind == QStringLiteral("steamgame")
                                   || r.kind == QStringLiteral("epicgame") || r.kind == QStringLiteral("goggame")
                                   || r.kind == QStringLiteral("battlenetgame")));
        if (!kind.isEmpty() && !match) continue;
        if (!system.isEmpty() && r.system != system) continue; // per-console scope
        MediaItem it;
        it.url = r.path;                                       // re-open target
        it.id = r.key;                                         // stable resume key (streamed items)
        it.mime = r.kind;                                      // routing kind
        it.type = iconTypeForKind(r.kind);                    // drives the placeholder icon + resume bar
        // Offline-first artwork: a downloaded item's locally cached poster wins over the remote url.
        it.thumbnailUrl = MetaCache::displayImage(it.id.isEmpty() ? it.url : it.id, r.thumb);
        it.title = r.title.isEmpty() ? QFileInfo(r.path).completeBaseName() : r.title;
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

MediaCatalog downloadsCatalog(const QList<DownloadedItem>& all, const QString& marker,
                               const std::function<bool(const QString&)>& fileExists)
{
    // marker = "<kind>|<system>": kind filters the catalogue; system (a SystemCatalog id, or "pc") scopes a
    // games console. An empty system matches any (non-game catalogues).
    const QString kind = marker.section(QLatin1Char('|'), 0, 0);
    const QString system = marker.section(QLatin1Char('|'), 1, 1);
    MediaCatalog cat; cat.title = QObject::tr("Downloaded");
    for (const DownloadedItem& d : all)
    {
        if (!kind.isEmpty() && d.kind != kind) continue;
        if (!system.isEmpty() && d.system != system) continue;
        const bool exists = fileExists ? fileExists(d.path) : QFileInfo::exists(d.path);
        if (!exists) continue; // hide entries whose file was deleted outside the app
        MediaItem it;
        it.url = d.path;
        it.id = d.key;
        it.mime = d.kind;                       // routing kind (openRecent dispatches on it)
        it.type = iconTypeForKind(d.kind);
        // Offline-first artwork: the locally cached poster wins over the remote url.
        it.thumbnailUrl = MetaCache::displayImage(it.id.isEmpty() ? it.url : it.id, d.thumb);
        it.title = d.title.isEmpty() ? QFileInfo(d.path).completeBaseName() : d.title;
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

MediaCatalog localLibraryCatalog(const QVector<LocalLibrary::VideoEntry>& entries)
{
    MediaCatalog cat; cat.title = QObject::tr("Local Library");
    for (const LocalLibrary::VideoEntry& e : entries)
    {
        MediaItem it;
        it.url = e.path;
        it.mime = QStringLiteral("local:video");
        it.type = QStringLiteral("movie");                 // both movies and episodes render as video tiles
        it.id = e.imdbId.isEmpty() ? (QStringLiteral("local:") + e.path) : e.imdbId;
        // Give subtitle matching an exact IMDB key (armSubtitleFetch reads imdbStreamId, not id): a movie's
        // own tt id, or "<seriesTt>:<season>:<episode>" for an episode — the format SubtitleFetcher parses
        // into parent_imdb_id/season_number/episode_number. Empty when unknown ⇒ the title-query fallback.
        if (e.kind == LocalLibrary::Kind::Movie) it.imdbStreamId = e.imdbId;
        else if (!e.seriesImdbId.isEmpty())
            it.imdbStreamId = e.seriesImdbId + QStringLiteral(":")
                            + QString::number(e.season) + QStringLiteral(":") + QString::number(e.episode);
        it.title = LocalLibrary::displayTitle(e);
        it.subtitle = e.plot;
        // Offline-first: a local NFO <thumb> is a file path; MetaCache::displayImage serves it if present.
        it.thumbnailUrl = MetaCache::displayImage(it.id, e.thumbPath);
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

MediaCatalog favoritesCatalog(const QList<FavoriteItem>& all, const QString& system)
{
    MediaCatalog cat; cat.title = QObject::tr("Favorites");
    for (const FavoriteItem& f : all)
    {
        // A merged PC game ("pcgame:<key>") has no path ON PURPOSE — which copy runs is decided at
        // activation — so the path test alone would drop every starred PC game out of the PC console's
        // ★ Favorites folder while Home still showed it, which reads as the star having failed. Its id
        // names the game, which is all the re-open needs.
        const bool mergedPc = f.itemId.startsWith(QStringLiteral("pcgame:"));
        if (f.path.isEmpty() && !mergedPc) continue;    // only local games have a per-console home
        if (!system.isEmpty() && f.system != system) continue;
        MediaItem it;
        it.url = f.path;
        it.id = f.itemId;
        it.mime = f.kind.isEmpty() ? QStringLiteral("game") : f.kind; // routing kind -> the game action menu
        it.type = iconTypeForKind(it.mime);
        // Offline-first artwork: the locally cached poster wins over the remote url.
        it.thumbnailUrl = MetaCache::displayImage(it.id.isEmpty() ? it.url : it.id, f.thumbnailUrl);
        it.title = f.title.isEmpty() ? QFileInfo(f.path).completeBaseName() : f.title;
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

FavoriteItem localGameFavorite(const MediaItem& it, const QString& systemHint)
{
    FavoriteItem f;
    f.itemId = it.id.isEmpty() ? it.url : it.id; // gameFavId rule: stable key, else path
    f.title = it.title;
    f.type = QStringLiteral("game");
    f.thumbnailUrl = it.thumbnailUrl;
    f.path = it.url;   // re-open by path (openFavorite recovers the console from the stores)
    f.kind = it.mime;  // "game" | "pcgame" (openRecent routing kind)
    // A merged PC game is in NO store and has no path, so neither the caller's hint nor deriveSystem can
    // say where it lives; its id already does. Without this it would be stamped system-less and vanish
    // from the PC console's ★ Favorites folder.
    if (f.itemId.startsWith(QStringLiteral("pcgame:"))) f.system = QStringLiteral("pc");
    else f.system = systemHint.isEmpty() ? FavoritesStore::deriveSystem(f.path, f.kind) : systemHint;
    return f;
}

MediaCatalog playlistsCatalog(const QList<Playlist>& all, const QString& categoryKey)
{
    MediaCatalog cat; cat.title = QObject::tr("Playlists");
    for (const Playlist& p : all)
    {
        MediaItem it;
        it.id = QStringLiteral("pl:") + p.id;
        it.type = QStringLiteral("_playlist");
        it.title = p.name;
        it.subtitle = QObject::tr("%n item(s)", "", int(p.items.size()));
        it.expandable = true;
        it.mime = QStringLiteral("playlist:") + p.id;
        cat.items.push_back(it);
    }
    MediaItem add; // a New-playlist entry at the bottom (activates -> name prompt, creating in this category)
    add.id = QStringLiteral("_newplaylist");
    add.type = QStringLiteral("_newplaylist");
    add.title = QObject::tr("➕  New playlist…");
    add.mime = QStringLiteral("newplaylist:") + categoryKey;
    cat.items.push_back(add);
    cat.hasMore = false;
    return cat;
}

MediaCatalog playlistItemsCatalog(const Playlist& p)
{
    MediaCatalog cat; cat.title = p.name;
    for (const PlaylistEntry& e : p.items)
    {
        MediaItem it;
        it.id = e.itemId; it.type = e.type; it.title = e.title; it.subtitle = e.subtitle;
        it.thumbnailUrl = e.thumbnailUrl; it.expandable = e.expandable;
        it.sourceAddonId = e.addonId; // per-entry addon resolution (mixed-source category playlists)
        // Store-game entries mirror their console tiles so a game added to a playlist still LAUNCHES (dead before):
        //   steam: / epic: — no url; the leaf-open builds the launcher URI from the id (steam://, com.epicgames…)
        //   gog:          — a DRM-free exe; the resolved exe was persisted into the entry's path at add-time and
        //                   rides back onto the tile's url, so the MONITORED launchPcExe path can run it.
        //   bnet:         — BOTH Battle.net routes take the GOG-shaped line: a code-less game's exe rides in the
        //                   path, and a coded game's url was empty at add-time so the emptiness rides back too
        //                   (an empty url is exactly what the battlenet:// URI launch expects).
        if (e.itemId.startsWith(QStringLiteral("steam:")))     it.mime = QStringLiteral("steamgame"); // launch natively
        else if (e.itemId.startsWith(QStringLiteral("epic:"))) it.mime = QStringLiteral("epicgame");  // launch via URI
        else if (e.itemId.startsWith(QStringLiteral("gog:")))  { it.mime = QStringLiteral("goggame"); it.url = e.path; } // exe rides in path
        else if (e.itemId.startsWith(QStringLiteral("bnet:"))) { it.mime = QStringLiteral("battlenetgame"); it.url = e.path; } // exe (code-less) or empty (coded)
        else if (!e.path.isEmpty()) { it.url = e.path; it.mime = QStringLiteral("localgame:") + e.kind; } // local game -> re-open by path
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

// ---- The merged PC Games folder -------------------------------------------------------------------------
//
// A STANDING RULE for everything below: no EpicLibrary:: or
// BattleNetLibrary:: call may appear below. probe_browse / probe_locallib / probe_perf compile this file
// WITHOUT those two .cpp files, so a call here is a CI-only link break — this repo has been bitten by exactly
// that twice. The two protocol URIs are therefore built INLINE, mirroring EpicLibrary::launchUrl and
// BattleNetLibrary::launchUri; probe_importers, which does link both, asserts the inline forms still equal the
// canonical helpers, so the copies cannot drift silently. SteamLibrary IS linked into every target that
// compiles this file, so its helpers are called normally.
namespace {

// Is this source a LAUNCHER copy (a store's own installed/owned entry) rather than a downloaded/addon one?
// The distinction is `kind`, NOT whether `launcher` happens to be set: PcGameStore is free to stamp the
// launcher a downloaded copy came from, and a rule that read `launcher` alone would then treat a scene
// release as if it were the Steam library entry.
bool isLauncherSource(const pcgame::PcGameSource& s)
{
    return s.kind == pcgame::PcGameSource::LauncherInstalled
        || s.kind == pcgame::PcGameSource::LauncherOwned;
}

// Launcher precedence for the DISPLAY TITLE only — it decides nothing about grouping or launching. A store's
// own name is a curated product name; a downloaded copy's name is a release name with scene tokens in it, so
// it loses to every launcher. The order is FIXED rather than "whichever we saw first" so the folder cannot
// reshuffle between runs when a launcher scan comes back in a different order.
//
// It keys on KIND FIRST and only then on `launcher`, so "a downloaded copy loses to every launcher" is a
// property of the code and not a coincidence of `launcher` happening to be empty on those sources. A
// Downloaded source carrying launcher = "steam" ranks last, exactly like one carrying nothing.
int pcTitleRank(const pcgame::PcGameSource& s)
{
    if (!isLauncherSource(s))                       return 4;   // a downloaded / addon copy: a release name
    if (s.launcher == QStringLiteral("steam"))      return 0;
    if (s.launcher == QStringLiteral("epic"))       return 1;
    if (s.launcher == QStringLiteral("gog"))        return 2;
    if (s.launcher == QStringLiteral("battlenet"))  return 3;
    return 4;   // a launcher copy from a store we have no precedence for
}

// One game while it is being assembled: its merged id, the best title seen so far, every title that
// contributed (so a search can match the GOG spelling of a game whose tile shows the Steam one), and its
// sources.
struct PcGroup
{
    QString     id;
    QString     title;
    int         bestRank = 99;
    QStringList titles;
    QVector<pcgame::PcGameSource> sources;
    // Parallel to `sources`: the title THAT source was added under. Kept because the merge is lossy on
    // purpose — the year strip fuses "Prey (2006)" with "Prey (2017)" — and this is the only place the
    // difference between two same-launcher copies still exists once they are in one group.
    QStringList sourceTitles;
};

} // namespace

MediaCatalog pcGamesCatalog(const QList<SteamGame>& steam, const QList<EpicGame>& epic,
                            const QList<GogGame>& gog, const QList<BattleNetGame>& bnet,
                            const QVector<pcgame::PcGameSource>& downloaded,
                            const QString& query, const QString& launcherFilter,
                            const std::function<QString(const QVector<pcgame::PcGameSource>&)>& poster,
                            const QList<SteamGame>& steamOwned)
{
    const QString q = query.trimmed();
    MediaCatalog cat;
    cat.title = q.isEmpty() ? QObject::tr("PC Games") : QObject::tr("PC Games · %1").arg(q);

    QVector<PcGroup>    groups;
    QHash<QString, int> byId;   // merged id -> index into groups; grouping IS equality of this id

    auto add = [&groups, &byId](const QString& rawTitle, int rank, const pcgame::PcGameSource& s)
    {
        const QString title = rawTitle.trimmed();

        // The merged identity — from pcgame::effectiveItemId and NOWHERE else. This id is what the user's
        // favourite, marks, play time and resume position are stored under, and PcGameRemap moves those
        // records onto the id that same function returns; building it here by hand is how the two came
        // apart before, so the arithmetic lives in one place and both sides call it. probe_browse pins
        // the equality.
        //
        // effectiveItemId rather than itemId is what SPENDS the user's merge overrides: with no verdict
        // recorded the two are the same string, and with one it is this call that separates a wrongly
        // merged key into an entry per copy, or fuses two entries the heuristic left apart. Grouping is
        // still "equal id" and nothing else — the escape hatch changes the id, not the grouping rule.
        //
        // An empty id means "nothing to group on" (a nameless copy), and such a copy is DROPPED rather
        // than bucketed: every nameless entry would otherwise share one id and fuse into a single tile.
        const QString id = pcgame::effectiveItemId(title);
        if (id.isEmpty()) return;

        int idx = byId.value(id, -1);
        if (idx < 0)
        {
            PcGroup g; g.id = id;
            groups.push_back(g);
            idx = int(groups.size()) - 1;
            byId.insert(id, idx);
        }
        PcGroup& g = groups[idx];
        g.titles << title;
        // Better rank wins; at equal rank the smaller string wins, which prefers the base title over its
        // edition variant ("Portal 2" < "Portal 2 - Game of the Year Edition") and is a total order, so the
        // result does not depend on the order the launcher scans came back in.
        if (rank < g.bestRank || (rank == g.bestRank && QString::compare(title, g.title) < 0))
        {
            g.title    = title;
            g.bestRank = rank;
        }
        // Carry the launcher's OWN name onto the source. RAW, not `title`: the pre-merge id built from it
        // (pcgame::legacyLaunchId, for a launcher with no id of its own) has to be byte-identical to the
        // candidate populatePcGames feeds remapTable, and that candidate is the launcher's name verbatim.
        // Trimming here would silently produce a launch id the remap never migrates.
        pcgame::PcGameSource named = s;
        if (named.sourceName.isEmpty()) named.sourceName = rawTitle;
        g.sources.push_back(named);
        g.sourceTitles << title;
    };

    QSet<QString> installedSteamIds;
    for (const SteamGame& g : steam)
    {
        installedSteamIds.insert(g.appid);
        pcgame::PcGameSource s;
        s.kind      = pcgame::PcGameSource::LauncherInstalled;
        s.launcher  = QStringLiteral("steam");
        s.launchId  = g.appid;
        s.launchUrl = SteamLibrary::launchUrl(g.appid);   // steam://rungameid/<appid>
        s.label     = QObject::tr("Steam");
        s.ready     = true;
        add(g.name, pcTitleRank(s), s);
    }
    // Owned on Steam but not installed (creds-gated; empty without a Web API key + SteamID). NOT ready: it
    // cannot be played without a multi-gigabyte download first, and pickAutoSource exists precisely so a
    // single Play keypress can never start one. It still carries a real launch — steam://install/<appid> —
    // so CHOOSING its row in the picker hands the install to the Steam client, which is the user asking.
    // An appid already installed is skipped: the installed source is strictly better and both would
    // otherwise sit in the same group as two Steam rows.
    for (const SteamGame& g : steamOwned)
    {
        if (installedSteamIds.contains(g.appid)) continue;
        pcgame::PcGameSource s;
        s.kind      = pcgame::PcGameSource::LauncherOwned;
        s.launcher  = QStringLiteral("steam");
        s.launchId  = g.appid;
        s.launchUrl = SteamLibrary::installUrl(g.appid);  // steam://install/<appid>
        s.label     = QObject::tr("Steam · not installed (install first)");
        s.ready     = false;
        add(g.name, pcTitleRank(s), s);
    }
    for (const EpicGame& g : epic)
    {
        pcgame::PcGameSource s;
        s.kind     = pcgame::PcGameSource::LauncherInstalled;
        s.launcher = QStringLiteral("epic");
        s.launchId = g.appName;
        // Mirrors EpicLibrary::launchUrl — see the link-break note above; probe_importers pins the equality.
        s.launchUrl = QStringLiteral("com.epicgames.launcher://apps/") + g.appName
                    + QStringLiteral("?action=launch&silent=true");
        s.label = QObject::tr("Epic Games");
        s.ready = true;
        add(g.name, pcTitleRank(s), s);
    }
    for (const GogGame& g : gog)
    {
        pcgame::PcGameSource s;
        s.kind     = pcgame::PcGameSource::LauncherInstalled;
        s.launcher = QStringLiteral("gog");
        s.launchId = g.id;
        s.exePath  = g.exe;                 // DRM-free: the monitored launchPcExe path, not a URI
        s.label    = QObject::tr("GOG");
        s.ready    = !g.exe.isEmpty();
        add(g.name, pcTitleRank(s), s);
    }
    for (const BattleNetGame& g : bnet)
    {
        pcgame::PcGameSource s;
        s.kind     = pcgame::PcGameSource::LauncherInstalled;
        s.launcher = QStringLiteral("battlenet");
        s.launchId = g.code;
        if (!g.code.isEmpty())
        {
            // Mirrors BattleNetLibrary::launchUri — see the link-break note above.
            s.launchUrl = QStringLiteral("battlenet://") + g.code;
            s.label     = QObject::tr("Battle.net");
            s.ready     = true;
        }
        else
        {
            // No product code means no protocol launch, only a guessed exe under the install dir. It is the
            // least reliable source there is, so it says so instead of sitting at parity with a real launch —
            // and with no exe either there is nothing to run at all, so it is NOT ready and pickAutoSource
            // can never hand Play a row that silently does nothing.
            s.exePath = g.exe;
            s.ready   = !g.exe.isEmpty();
            s.label   = g.exe.isEmpty() ? QObject::tr("Battle.net · no launch found")
                                        : QObject::tr("Battle.net · best-effort exe (may not launch)");
        }
        add(g.name, pcTitleRank(s), s);
    }
    // Already-built sources (a downloaded copy from PcGameStore). Its label is its title AND its picker row;
    // the label is kept verbatim rather than rewritten — it is the caller's text, and the release name is
    // exactly what tells two downloaded copies apart in the picker.
    for (const pcgame::PcGameSource& s : downloaded)
        add(s.label, pcTitleRank(s), s);

    // The normalised query. A query that normalises to NOTHING ("!!!", "GOTY") would otherwise be a substring
    // of every title and match the whole library, so it falls back to a plain case-insensitive match.
    const QString qn = pcgame::normalizeTitle(q);

    for (PcGroup& g : groups)
    {
        if (!launcherFilter.isEmpty())
        {
            // "HAS a LAUNCHER source for this launcher" — isLauncherSource, not just a matching `launcher`
            // string, so a downloaded copy that happens to record where it came from does not make the game
            // appear under "what I own on Steam". Owning it on Steam and having pirated it are not the same
            // claim, and the filter is the one that means the former.
            bool has = false;
            for (const pcgame::PcGameSource& s : g.sources)
                if (isLauncherSource(s) && s.launcher == launcherFilter) { has = true; break; }
            if (!has) continue;
        }
        if (!q.isEmpty())
        {
            bool hit = false;
            for (const QString& t : g.titles)
            {
                if (qn.isEmpty() ? t.contains(q, Qt::CaseInsensitive)
                                 : pcgame::normalizeTitle(t).contains(qn)) { hit = true; break; }
            }
            if (!hit) continue;
        }

        // DISAMBIGUATE same-launcher rows. The merge key is lossy on purpose (the trailing-year strip fuses
        // "Prey (2006)" with "Prey (2017)"), and when both copies come from the SAME launcher every field the
        // picker shows is identical: both labelled "Steam", both ready, so pickAutoSource returns -1 and the
        // menu offers two byte-identical rows. Nothing is lost — the launchIds differ, so both really do
        // launch the right copy — but the user cannot tell which is which, and the display title resolves to
        // the bare "Prey", so the remake is invisible.
        //
        // The fix re-attaches the per-launcher title, which is exactly what the strip removed and the only
        // field that still differs. Only LAUNCHER sources are counted (a downloaded copy's label is already
        // its own release name, and appending it would just print it twice), and a launcher that contributed
        // ONE source is left alone, so the common "Steam" / "GOG" rows read as plainly as before.
        {
            QHash<QString, int> perLauncher;
            for (const pcgame::PcGameSource& s : g.sources)
                if (isLauncherSource(s) && !s.launcher.isEmpty()) perLauncher[s.launcher] += 1;
            for (int i = 0; i < g.sources.size(); ++i)
            {
                pcgame::PcGameSource& s = g.sources[i];
                if (!isLauncherSource(s) || perLauncher.value(s.launcher) < 2) continue;
                const QString t = i < g.sourceTitles.size() ? g.sourceTitles.at(i) : QString();
                if (t.isEmpty() || s.label.contains(t)) continue;
                s.label = QObject::tr("%1 · %2").arg(s.label, t);
            }
            // Backstop: two copies can share a launcher AND a title (a store listing the same name twice).
            // The launchId is what the merge never touches, so it is the last thing guaranteed to differ.
            QHash<QString, int> labelCount;
            for (const pcgame::PcGameSource& s : g.sources) labelCount[s.label] += 1;
            for (pcgame::PcGameSource& s : g.sources)
            {
                if (labelCount.value(s.label) < 2) continue;
                const QString key = s.launchId.isEmpty() ? s.addonItemId : s.launchId;
                if (key.isEmpty()) continue;
                s.label = QObject::tr("%1 · %2").arg(s.label, key);
            }
        }

        // Ready first, then launcher name, then launchId, then label. stable_sort so two sources that are
        // equal on all four keep the order they were added in rather than shuffling per run.
        std::stable_sort(g.sources.begin(), g.sources.end(),
                         [](const pcgame::PcGameSource& a, const pcgame::PcGameSource& b) {
                             if (a.ready != b.ready) return a.ready;   // a ready row can be pressed NOW
                             int c = QString::compare(a.launcher, b.launcher);
                             if (c != 0) return c < 0;
                             c = QString::compare(a.launchId, b.launchId);
                             if (c != 0) return c < 0;
                             return QString::compare(a.label, b.label) < 0;
                         });

        MediaItem it;
        it.id         = g.id;
        it.mime       = QStringLiteral("pcgame");   // the ONE routing kind for a PC game
        it.type       = QStringLiteral("game");
        it.title      = g.title;
        it.systemHint = QStringLiteral("pc");       // the console this belongs to (favourites scope on it)
        it.pcSources  = g.sources;
        // The badge the Steam console used to carry. It says "not installed" only when NOTHING here is
        // installed — a game owned on Steam AND installed on GOG is installed, and badging it would be a
        // lie about the copy that actually runs. A not-READY installed source (a code-less Battle.net title
        // with no exe) is installed but unlaunchable, which is a different statement and gets no badge.
        {
            bool anyLocal = false;
            for (const pcgame::PcGameSource& s : g.sources)
                if (s.kind != pcgame::PcGameSource::LauncherOwned
                    && s.kind != pcgame::PcGameSource::AddonAvailable) { anyLocal = true; break; }
            if (!anyLocal && !g.sources.isEmpty()) it.subtitle = QObject::tr("Not installed");
        }
        // it.url stays EMPTY on purpose: WHICH copy runs is decided at activation by the source picker, and a
        // url here would make the generic "a file is associated" branch claim the tile first.
        if (poster) it.thumbnailUrl = poster(g.sources);
        else
            for (const pcgame::PcGameSource& s : g.sources)
                if (s.launcher == QStringLiteral("steam"))
                { it.thumbnailUrl = SteamLibrary::posterUrl(s.launchId); break; }
        cat.items.push_back(it);
    }

    // A total order on what the user reads, then on identity: two different games whose titles compare equal
    // still have different ids, so the folder never depends on the scan order.
    std::sort(cat.items.begin(), cat.items.end(), [](const MediaItem& a, const MediaItem& b) {
        const int c = QString::compare(a.title, b.title, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.id < b.id;
    });
    cat.hasMore = false;
    return cat;
}

// The Trakt "airing soon" list (#23). Pure like every builder above: entries in, MediaCatalog out — no
// TraktClient, no network, no ini. The SURFACE decides whether to show this at all (TraktClient::
// calendarAvailable()); this only decides what the list looks like once it is shown.
MediaCatalog traktCalendarCatalog(const QVector<CalendarEntry>& entries, const QDateTime& nowUtc)
{
    MediaCatalog cat; cat.title = QObject::tr("Airing Soon");
    // Future-only, then sorted. Both halves matter: the window Trakt was asked for starts TODAY, so its
    // first rows are episodes that already aired hours ago, and Trakt does not promise the array is
    // ordered. An invalid air time is dropped rather than sorted to one end — it cannot be placed on a
    // calendar at all, which is the same rule the parser applies (TraktRead.h).
    QVector<CalendarEntry> soon;
    soon.reserve(entries.size());
    for (const CalendarEntry& e : entries)
    {
        if (!e.airsAtUtc.isValid()) continue;
        if (e.airsAtUtc <= nowUtc) continue;   // the CLOSED past boundary — see the header
        soon.push_back(e);
    }
    // stable_sort, not sort: two episodes of the same show airing on the same tick (a double-bill, which
    // Trakt really does return) keep the order Trakt gave them instead of shuffling per run.
    std::stable_sort(soon.begin(), soon.end(),
                     [](const CalendarEntry& a, const CalendarEntry& b) { return a.airsAtUtc < b.airsAtUtc; });

    for (const CalendarEntry& e : soon)
    {
        MediaItem it;
        it.type = QStringLiteral("episode");
        it.title = e.showTitle;
        it.thumbnailUrl = e.posterUrl;
        it.mime = QStringLiteral("trakt:cal"); // marks the row on the Home list, which has no level context
        // "" is the DOCUMENTED "not playable" signal, never an error (TraktRead.h) — the row still ships.
        it.imdbStreamId = trakt::imdbStreamIdFor(e.showIds, e.season, e.episode);
        // "S01E04 · Tue 21 Jul". The DAY IS LOCAL — deliberately the one thing here that is not UTC.
        // Selection above is an INSTANT range and is correctly UTC; the day a viewer reads off a shelf is
        // a LOCAL-CALENDAR concept. A US prime-time episode airing Tuesday 21:00 ET is Wednesday 01:00
        // UTC, so a UTC-formatted day tells a New York viewer "Wed 22 Jul" for a Tuesday-night show —
        // the common case for US television, not an edge case, and unrecoverable by the user because no
        // clock time is printed beside it. probe_browse pins BOTH the format and the local conversion.
        const QString code = QStringLiteral("S%1E%2").arg(e.season, 2, 10, QLatin1Char('0'))
                                                     .arg(e.episode, 2, 10, QLatin1Char('0'));
        it.subtitle = code + QStringLiteral(" · ")
                    + e.airsAtUtc.toLocalTime().toString(QStringLiteral("ddd d MMM"));
        // Say so, rather than leaving a row that simply does nothing when pressed.
        if (it.imdbStreamId.isEmpty()) it.subtitle += QStringLiteral(" · ") + QObject::tr("No source");
        // Identity: the stream id when there is one, else a stable synthetic key from the show + episode,
        // so an unplayable row can still be focused, marked and re-selected across a rebuild.
        it.id = it.imdbStreamId.isEmpty()
                    ? QStringLiteral("trakt:%1:%2:%3").arg(e.showTitle).arg(e.season).arg(e.episode)
                    : it.imdbStreamId;
        // it.url stays EMPTY, always: the stream resolver fills it at play time from imdbStreamId. A url
        // here would make activateItem's generic "a file is associated" branch claim the row.
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}

} // namespace browse
