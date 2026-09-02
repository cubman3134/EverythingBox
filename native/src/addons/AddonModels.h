// Data models for the addon system, ported from the Unity AddonModels. An addon is a folder with a
// manifest.json + an entry script (main.js). A "media-source" addon's JS returns catalogs of MediaItems.
#pragma once
// NB on this include: it is NOT free. `core/` is not below `addons/` in the include graph — GamelistStore.h
// and MetaCache.h both include THIS header — so there is no layering rule being broken here, and no cycle
// (PcGameId.h is QtCore-only and includes nothing of ours). What it does mean is that PcGameId.h is now a
// TRANSITIVE dependency of nearly every TU in the app, so editing it rebuilds effectively the whole tree.
#include "../core/PcGameId.h"       // MediaItem::pcSources — the launch options on one merged PC game
#include "../core/RemoteAudiobook.h" // MediaItem::bookParts — the files a multi-file release is made of (#214)
#include "../core/StreamHeaders.h"  // MediaItem::requestHeaders — this source's proxyHeaders (QtCore-only)
#include "../comic/ChapterRun.h"    // MediaItem::chapterRun — the volumes either side of an opened issue
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class QJsonObject;

// A user-configurable addon setting (API key, base URL, toggle, ...) declared in the manifest. The app
// renders a form from these and stores the values per addon; the script reads them via getConfig(key).
struct AddonSetting
{
    QString key;          // config key the script queries
    QString label;        // display label
    QString type;         // "text" | "password" | "checkbox" | "number" (default: text)
    QString defaultValue; // used until the user sets a value
    QString description;   // optional help text
};

// A named, media-typed catalog an addon offers (Movies, TV Shows, Games, Music, ...). The script's
// getCatalog() receives the catalog id to know which to build.
struct AddonCatalog
{
    QString id;    // passed back to getCatalog({catalog:id})
    QString name;  // display label (the media-type tab)
    QString type;  // "movie" | "series" | "game" | "album" | ... (hint for routing/icons)
    // This catalog cannot answer without a search term, so it is a SEARCH SOURCE and not a browse shelf.
    // It must still be listed — dropping it is what previously made search-only add-ons invisible — so the
    // browse surfaces filter on this instead.
    bool searchOnly = false;
    // Non-empty when this catalog can never be asked at all: it REQUIRES an extra we have no value for (a
    // Stremio Unsatisfiable catalog). It is still listed, and opening it shows this reason as a single info
    // row — "skipped with a reason" has to reach the user somewhere, and a catalog that silently vanishes
    // from the shelf list is exactly the failure this whole translator exists to stop. Nothing may FETCH it.
    QString skipReason;
};

// A user-selectable catalog filter (genre / year / rating / sort), advertised by a catalog response so the
// UI can render the right dropdowns per screen. The first option is the "Any / default" choice.
struct CatalogFilter
{
    QString key;     // "genre" | "year" | "rating" | "sort" - sent back as the selected param
    QString label;   // dropdown label, e.g. "Genre"
    QVector<QPair<QString, QString>> options; // (value, label); value "" = no filter
};

// A media type an addon defines, so new types (beyond the built-ins) get their own visuals. The app keys
// a registry by `type`; a catalog/item of that type then uses this colour + icon. Built-in types still
// have hand-drawn defaults; these override/extend them. `openKind` ties an "Open a file" action to the type.
struct AddonMediaType
{
    QString type;         // e.g. "podcast" - matches a catalog/item type
    QString color;        // accent colour, e.g. "#E0662E"
    QString icon;         // an emoji glyph, OR a bundled image file in the addon folder ("icons/x.svg")
    QString openKind;     // "" | "video" | "audio" | "document" | "game" (offer an Open-a-file item)
    QString detailLayout; // detail-page arrangement: "" / "poster" (default) | "banner" | "text"
};

// A protocol RESOURCE an addon declares it can answer, with the media types it answers it for. The
// long-standing resources (catalog/meta/detail/stream) are implied by the manifest's catalogs and are not
// listed here; this array exists for the capabilities that are OPTIONAL and that the client must not ask
// for blind — today `chapters` and `pages` (issue #188).
//
// The never-ask rule: the client checks declares() BEFORE issuing a request. An addon that does not list a
// resource is never asked for it, so an unsatisfiable request cannot happen and an addon written before the
// resource existed is not broken by it — it simply keeps the older path (see AddonManager::requestDetail).
struct AddonResource
{
    QString name;       // "chapters" | "pages" (open-ended: an unknown name is parsed and ignored)
    QStringList types;  // the media types it answers for, e.g. ["manga"]. Empty = every type.
};

// One entry of a serial work: a chapter of a manga, an installment of a web novel, a part of a serial.
// Every field but `id` is optional — a source that knows only "this chapter exists and here is how to ask
// for its pages" still produces a usable list.
//
// `number` is a STRING, not a double. Sources publish "9.5", "10.1", "Extra", "Omake" and "" (a oneshot),
// and a double cannot hold the last three. The client orders by it with a natural sort (chapterLess), which
// puts "10" after "9.5" and un-numbered entries last, keeping source order between ties.
struct AddonChapter
{
    QString id;         // opaque; handed straight back as the `pages` resource's {chapterId}
    QString number;     // "1", "9.5", "Extra", "" — displayed and sorted on
    // The collection this chapter was published in (a volume / season / arc), as the source spells it.
    // Ordering is `number`'s job alone — this is a LABEL, so a series whose volumes restart their chapter
    // numbering still reads in the source's order rather than in a numbering the client invented.
    QString volume;
    QString title;      // the chapter's own title, when it has one
    QString language;   // BCP-47-ish tag as the source spells it ("en", "pt-br")
    QString group;      // scanlation group / translator / publisher
    QString published;  // ISO-8601 date or date-time, as the source spells it
    int pageCount = -1; // -1 = the source did not say
};

// One page of a chapter, in reading order.
//
// `headers` is the behaviorHints.proxyHeaders vocabulary (StreamHeaders::parseHeaderMap, the same hygiene
// rules): many image CDNs gate on a Referer, and without somewhere to put it a source can list pages that
// cannot be fetched. width/height are the source's own numbers where it publishes them (0 = unknown).
struct AddonPage
{
    QString url;
    int width = 0;
    int height = 0;
    StreamHeaders::Headers headers;
};

// The parsed `chapters` resource: `{ "chapters": [...], "hasMore": bool? }`. `hasMore` is optional and
// defaults to false — a source that returns a whole series at once simply omits it.
struct AddonChapterList
{
    QVector<AddonChapter> chapters;
    bool hasMore = false;
    static AddonChapterList fromJson(const QByteArray& json);
};

// The parsed `pages` resource: `{ "pages": [...] }`, already in reading order (the client does NOT reorder
// pages — only chapters have a sort rule).
struct AddonPageList
{
    QVector<AddonPage> pages;
    static AddonPageList fromJson(const QByteArray& json);
};

namespace AddonChapters
{
    // Natural order over two chapter NUMBERS, digit runs compared as numbers. "9.5" < "10" (the case a
    // plain string compare gets wrong, and the one #188 names), "1" < "1.5" < "2", and an entry with no
    // number sorts LAST — an "Extra"/"Omake" with no number is not chapter zero.
    bool numberLess(const QString& a, const QString& b);

    // Order a chapter list in place: STABLE, so ties (two scanlations of chapter 12, or two entries the
    // source numbered the same) keep the order the source listed them in. That is the whole ordering rule
    // the client applies; everything else about the list is the source's.
    void sortNaturally(QVector<AddonChapter>& chapters);
}

struct AddonManifest
{
    QString id;            // unique, reverse-DNS recommended
    QString name;          // display name
    QString version;
    QString type;          // e.g. "media-source"
    QString entry;         // script file name (default "main.js")
    QString author;
    QString description;
    QString accent;          // optional per-addon accent colour (hex), used by this addon's catalog types
    QStringList permissions; // declared capabilities, e.g. ["network"]
    QString minAppVersion;
    QString updateUrl;       // optional public URL (e.g. a GitHub raw link) to this addon's latest .addon
                             // package; when set, a JsLocal addon self-updates on startup if it's newer
    QVector<AddonSetting> settings;       // user-configurable credentials/options
    QVector<AddonCatalog> catalogs;       // media-typed catalogs (empty = a single implicit catalog)
    QVector<AddonMediaType> mediaTypes;   // custom media types with their own colour/icon
    // Media types this addon supplies AGGREGATABLE metadata/artwork for (e.g. ["game"]). A pure meta
    // provider (SteamGridDB / IGDB / ScreenScraper / TheGamesDB) declares this + empty catalogs: it never
    // shows as a browse source, but the host fans its getMeta() out on hover and merges it with the others.
    QStringList metaFor;
    // Optional protocol resources this addon answers (see AddonResource). Absent in every manifest written
    // before #188, which is exactly what the never-ask rule turns into "keep using the older path".
    QVector<AddonResource> resources;

    static AddonManifest fromJson(const QByteArray& json, bool* ok = nullptr);
    QString entryOrDefault() const { return entry.isEmpty() ? QStringLiteral("main.js") : entry; }
    // Does this addon declare `name` for `type`? A resource with no `types` answers for every type; an
    // empty `type` argument asks "for anything at all". THE gate every chapters/pages request passes.
    bool declares(const QString& name, const QString& type = QString()) const
    {
        for (const AddonResource& r : resources)
        {
            if (r.name != name) continue;
            if (type.isEmpty() || r.types.isEmpty() || r.types.contains(type)) return true;
        }
        return false;
    }
};

// Extensible artwork + preview media + free-form metadata for an item. Every field is optional: a theme
// that binds to an absent role simply renders its default (Theme.js already degrades a missing binding to
// the element's fallback). New metadata providers can add image roles, videos, audio or meta keys with NO
// code change here — fromJson passes unknown roles/keys through verbatim, and so does toVariant().
struct MediaArt
{
    // role -> ordered candidate URLs, best first. Conventional roles: "poster", "box", "logo", "clearlogo",
    // "hero", "banner", "fanart", "background", "screenshot", "disc", "thumb", "icon", and "manual" (a scraped
    // game manual PDF/CBZ, ScreenScraper being the primary supplier). Open-ended.
    // NB "manual" is a first-class role for merge/precedence purposes, but it is NOT an image: it is megabytes,
    // so MetaCache treats it as an on-demand role (MetaCache::isOnDemandRole) — recorded in the bundle, fetched
    // only on explicit open (MetaCache::fetchManual), and deliberately EXCLUDED from saveArt's eager prefetch.
    QMap<QString, QStringList> images;
    QStringList videos;   // preview / trailer clip URLs, best first
    QStringList audio;    // theme song / preview music URLs, best first
    QVariantMap meta;     // arbitrary extra metadata (developer, players, esrb, ...); providers add freely

    bool isEmpty() const { return images.isEmpty() && videos.isEmpty() && audio.isEmpty() && meta.isEmpty(); }
    QString image(const QString& role) const   // first (best) url for a role, else ""
    {
        const auto it = images.constFind(role);
        return (it != images.constEnd() && !it->isEmpty()) ? it->first() : QString();
    }
    void addImage(const QString& role, const QString& url); // append a candidate (dedup, best-first order kept)

    // Merge another source in at LOWER precedence: keep every candidate/role/video/meta we already have and
    // append this source's extras after ours. The game aggregator calls this in priority order (best first).
    void mergeLowerPriority(const MediaArt& other);

    static MediaArt fromJson(const QJsonObject& o); // parse images/videos/audio/meta (+ flat role keys)
    QVariantMap toVariant() const;                  // { images:{role:[urls]}, videos, audio, meta }
    // Write the art into a themed item map: the `images/videos/audio/meta` sub-objects PLUS a scalar alias
    // per role (selected.logo, selected.box, ... = that role's best url) for simple theme bindings. Never
    // clobbers a key the row already holds (title/type/image/...), so reserved fields stay put.
    void writeInto(QVariantMap& row) const;
};

struct MediaItem
{
    QString id;            // opaque id the addon uses for drill-down (getDetail)
    QString title;
    QString subtitle;
    QString type;          // "movie", "series", "season", "episode", "game", "album", "track", "ebook", ...
    QString thumbnailUrl;  // poster/cover image (http) to show in the grid
    QString url;           // playable location (file/http) - empty until a file is associated
    QString mime;
    // HTTP request headers `url` needs (behaviorHints.proxyHeaders.request from a Stremio stream). They are
    // bound to THIS url — anything that replaces the url must re-derive them via StreamHeaders::forPlayUrl,
    // which drops them when the origin changes. Never serialized, never written to Recent/Resume: they are
    // per-source, frequently token-bearing, and a stale one is a leak.
    StreamHeaders::Headers requestHeaders;
    bool expandable = false; // a container (series/season/album): clicking fetches its children via getDetail
    // HOW FAR THROUGH THIS ROW IS, 0..1 — or < 0 for "look it up the usual way" (issue #139 increment 2).
    //
    // The surface's continue-watching bar normally comes from the RESUME STORE, keyed by the row's own stable
    // id: press play on a film, the position is banked under that id, and the tile finds it again. A local
    // AUDIOBOOK breaks that identity and cannot be made to fit it — the player writes one mark per FILE, a
    // book is a folder of files, and its progress is a sum over their marks that is filed under none of them.
    // So a book row carries its own answer here rather than a second resume key being invented for it (which
    // would then have to be kept in step with the marks the player actually writes, and would be free to
    // disagree with where "Play book" resumes).
    //
    // DEFAULT -1 IS "SAY NOTHING NEW": every other row leaves it alone and takes the store lookup exactly as
    // it always did. Not serialized — it is recomputed with the row it belongs to.
    double progress = -1.0;
    // THE CONTAINER THIS ITEM BELONGS TO, as an id the same addon will answer for ("comicvine:volume:1234"
    // for an issue). Empty for everything with no meaningful parent, which is most items. It exists so a
    // leaf can be asked "what else is in your series?" WITHOUT the browse surface that listed it — which
    // is the difference between a resumed comic having a next volume and not having one.
    QString parentId;
    // ...and what that container is CALLED. It travels with the id because the two are wanted together and
    // are known together: a run rebuilt from `parentId` searches a file provider by the series NAME, and
    // the only other place to get one is the children response's own title — which is a catalog heading
    // ("Issues"), not a series. Empty whenever parentId is.
    QString parentTitle;
    // Set when a file provider (Allarr) resolved this playable and can serve an alternate source on demand
    // (its /stream supports ?n=K). Drives the player/reader's "Issue with Streaming" button. Not serialized.
    bool nextSourceCapable = false;
    // For games: the console/platform this was opened from (e.g. "PSP", "GameCube"). Lets the launcher pick
    // the right emulator even when the file extension is shared (PSP .iso vs GameCube .iso). Not serialized.
    QString systemHint;
    // url is a Cloudflare-gated direct source (lolroms) to fetch with a browser-UA curl rather than the normal
    // HTTP client (whose TLS fingerprint gets a 403). Set by resolveStream on desktop only. Not serialized.
    bool cfCurl = false;
    // The source addon this item came from, set when it's surfaced outside its own catalog (a cross-addon search
    // merges results from many addons into one grid) so it can be re-opened through the right addon. Not serialized.
    QString sourceAddonId;
    // #224: WHERE A FRESH LINK FOR THIS PLAYABLE COMES FROM, when that is not `sourceAddonId` + `id`.
    //
    // The Recents re-mint recipe is written from the item that played, and for almost every route the item
    // that played is the one the resolving addon knows: browse a provider's shelf, and the row you pressed
    // IS its release. The doc-bridge breaks that identity — a title pressed on a METADATA shelf (Google
    // Books, Comic Vine) is searched for BY NAME on a file provider, so what reaches the play sink is the
    // catalog's item, whose id ("googlebooks:…") means nothing to the provider and whose addon cannot
    // resolve a stream at all. A recipe written from it named a metadata-only JsLocal addon, and re-opening
    // the row answered "Couldn't get a fresh link" the instant it was pressed — resolveStream refuses a
    // non-RemoteHttp source on its first line, so there was not even a request to fail.
    //
    // These two say "ask THAT addon for THIS id instead", and applyRemintRecipe prefers them when set.
    // They do NOT replace sourceAddonId: that field is load-bearing for the catalog-side questions a row
    // still has to ask (rebuilding a comic's chapter run asks the CATALOG what series an issue belongs to),
    // and the two answers are genuinely different addons on this route.
    //
    // The item's own `id` stays the CATALOG's id deliberately, because it is the key everything user-facing
    // is filed under — the Recents row, the resume position, and an audiobook's part tokens (bookKey +
    // fileName). Re-minting by the release id while keying by the catalog id is the whole point: the same
    // release comes back, so the same part names come back, so the listener lands where they left off.
    // Not serialized (the RECIPE is what persists, in RecentItem's four fields).
    QString remintAddonId;
    QString remintItemId;
    // The IMDB stream id this playable was resolved from - "tt123" (movie) or "ttShow:season:episode" (episode).
    // Carried to the player so it can auto-fetch a matching subtitle from OpenSubtitles. Not serialized.
    QString imdbStreamId;
    // Alternate / original titles for this item (e.g. IGDB alternative_names: the Japanese original, regional
    // rebrands like "Rockman"/"Mega Man" or "Probotector"/"Contra"). Used to retry a ROM/file-provider lookup
    // when the localized catalog title doesn't match the copy's original name. Not serialized.
    QStringList altNames;
    // External-player one-off routing hint carried from a detail action THROUGH the async resolve chain to
    // the play emit (rides the item, so a failed/abandoned resolve can't leak the force onto a later play):
    // 0=default, 1=force built-in, 2=force external. Set only for a themed "Open in external player"/"Play with
    // built-in player" one-off on a catalog leaf; read by MainWindow::openLibraryItem. Not serialized.
    int playRouteHint = 0;
    // Extra artwork/videos/audio/metadata beyond the single grid `thumbnailUrl` (logo, box, fanart,
    // screenshots, preview clips, theme music, provider facts). Optional; filled by richer providers and the
    // game-metadata aggregator. Threaded into the themed item map so themes can bind selected.logo etc.
    MediaArt art;
    // Every way to launch this game, when the item is one merged PC Games entry (mime "pcgame"): the Steam /
    // Epic / GOG / Battle.net copies and any downloaded one, all of the SAME game. `id` is the identity and
    // `url` stays EMPTY — the launch lives here, and pcgame::pickAutoSource decides between them (or the
    // picker asks). Empty for every other kind of item. Not serialized.
    //
    // It is a field rather than a re-derivation because the merge is what the folder just computed: rebuilding
    // it at activation time would be a second copy of the grouping rule, free to disagree with the tile the
    // user actually pressed.
    QVector<pcgame::PcGameSource> pcSources;
    // THE FILES THIS RELEASE IS MADE OF, already filtered to audio and already in the order they are
    // meant to be heard (#214). Non-empty only for an audiobook leaf whose source could enumerate the
    // release; EMPTY for everything else, which is what makes every existing route read `url` exactly
    // as it always did.
    //
    // It is a FIELD, for the reason pcSources is one: the resolve that fetched this list is the only
    // place the list exists, and re-deriving it at open time would be a second network conversation
    // free to disagree with the one the user is already waiting on. And it rides the ITEM rather than a
    // parallel signal because openItem is the ONE door into MainWindow::openLibraryItem — a second door
    // is a thing the classic surface and the themed surface would each have to learn, which is the
    // shape of every routing bug LeafRoute.h was written to end.
    //
    // NOT SERIALIZED, and it must never be: a Part carries the source's item id for one file, which is
    // meaningful only to the source that minted it and only for as long as that release is what it was.
    QVector<RemoteAudiobook::Part> bookParts;
    // THE VOLUMES EITHER SIDE OF THIS ONE, when it was opened from a list that knew them. Empty for
    // everything else, which is what leaves every other open behaving exactly as it did.
    //
    // It rides the ITEM for the reason bookParts does, stated just above: openItem is the ONE door into
    // MainWindow::openLibraryItem, and a parallel signal carrying half of an open would be a second.
    // Never serialized — a Recent rebuilds its run from parentId instead.
    ChapterRun chapterRun;
};

struct MediaCatalog
{
    QString title;
    QVector<MediaItem> items;
    bool hasMore = false; // the addon reports another page is available (drives infinite scroll)
    QVector<CatalogFilter> filters; // filters this catalog supports (drives the per-screen filter dropdowns)

    static MediaCatalog fromJson(const QByteArray& json);
};

// Metadata about a single item, returned by an addon's getMeta(). Drives the detail-page header:
// a cover image, a title/subtitle, a set of labelled facts (Rating, Genres, Runtime, ...) and a synopsis.
struct MediaFact { QString label; QString value; };

// The ONE display join for a facts list: "Label: value     •     Label: value". Takes the QVariantList-of-
// {label,value} shape the themed panels publish (not QVector<MediaFact>), because that is the shape every
// producer hands to the QML side. Both metadata surfaces carry the joined scalar under `factsText` — the
// hover panel (selectedMeta) and the detail page (selected/detailData) — so a theme binding `factsText`
// means the same thing on either. A theme.json cannot join a list itself: `text` renders a scalar only.
inline QString joinFactsText(const QVariantList& facts)
{
    QStringList out;
    for (const QVariant& fv : facts)
    {
        const QVariantMap fm = fv.toMap();
        const QString l = fm.value(QStringLiteral("label")).toString();
        const QString v = fm.value(QStringLiteral("value")).toString();
        if (!v.isEmpty()) out << (l.isEmpty() ? v : (l + QStringLiteral(": ") + v));
    }
    return out.join(QStringLiteral("     •     "));
}

struct MediaDetail
{
    QString title;
    QString subtitle;
    QString overview;          // synopsis / description (may contain plain text)
    QString imageUrl;          // larger cover/poster (http or local)
    QVector<MediaFact> facts;  // labelled key/value rows
    // Stremio stream id for this item, when the addon can supply one (e.g. a TMDB catalog mapping to IMDB):
    // "tt123" for a movie, "ttShow:season:episode" for an episode. Lets stream addons (Torrentio/Allarr)
    // resolve a playable source for a catalog whose own ids aren't IMDB.
    QString imdbStreamId;
    // Same meaning as MediaItem::parentId / parentTitle. A resumed item carries an id and nothing else,
    // so its parent has to come back from /meta; this is where it arrives.
    QString parentId;
    QString parentTitle;
    bool valid = false;        // false = addon returned nothing usable (header stays hidden)
    // Rich artwork/videos/audio/metadata for the detail + themed live panel (logo, box, fanart gallery,
    // trailers, theme music, extra facts). Optional; the single imageUrl above stays the primary cover.
    MediaArt art;

    static MediaDetail fromJson(const QByteArray& json);
};
