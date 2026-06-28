// Data models for the addon system, ported from the Unity AddonModels. An addon is a folder with a
// manifest.json + an entry script (main.js). A "media-source" addon's JS returns catalogs of MediaItems.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

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
    QVector<AddonSetting> settings;       // user-configurable credentials/options
    QVector<AddonCatalog> catalogs;       // media-typed catalogs (empty = a single implicit catalog)
    QVector<AddonMediaType> mediaTypes;   // custom media types with their own colour/icon

    static AddonManifest fromJson(const QByteArray& json, bool* ok = nullptr);
    QString entryOrDefault() const { return entry.isEmpty() ? QStringLiteral("main.js") : entry; }
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

    static MediaDetail fromJson(const QByteArray& json);
};
