// Data models for the addon system, ported from the Unity AddonModels. An addon is a folder with a
// manifest.json + an entry script (main.js). A "media-source" addon's JS returns catalogs of MediaItems.
#pragma once
#include <QMap>
#include <QString>
#include <QStringList>
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

    static AddonManifest fromJson(const QByteArray& json, bool* ok = nullptr);
    QString entryOrDefault() const { return entry.isEmpty() ? QStringLiteral("main.js") : entry; }
};

// Extensible artwork + preview media + free-form metadata for an item. Every field is optional: a theme
// that binds to an absent role simply renders its default (Theme.js already degrades a missing binding to
// the element's fallback). New metadata providers can add image roles, videos, audio or meta keys with NO
// code change here — fromJson passes unknown roles/keys through verbatim, and so does toVariant().
struct MediaArt
{
    // role -> ordered candidate URLs, best first. Conventional roles: "poster", "box", "logo", "clearlogo",
    // "hero", "banner", "fanart", "background", "screenshot", "disc", "thumb", "icon". Open-ended.
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
    bool expandable = false; // a container (series/season/album): clicking fetches its children via getDetail
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
    bool valid = false;        // false = addon returned nothing usable (header stays hidden)
    // Rich artwork/videos/audio/metadata for the detail + themed live panel (logo, box, fanart gallery,
    // trailers, theme music, extra facts). Optional; the single imageUrl above stays the primary cover.
    MediaArt art;

    static MediaDetail fromJson(const QByteArray& json);
};
