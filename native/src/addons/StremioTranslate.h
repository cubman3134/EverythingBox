// Translates the Stremio addon protocol into MMV's models. Pure: JSON in, structs out — no network, no
// QSettings, no widgets, so probe_stremio can assert every rule against real manifest fixtures.
//
// This lives apart from AddonManager deliberately. The Stremio support that preceded it was written inline
// in a ~1500-line networking class and was incomplete BECAUSE it was untestable: required-extra catalogs
// were dropped, idPrefixes was never read, and stream titles were parsed away. Keeping the rules here, in
// a unit with no I/O, is what lets them be pinned.
#pragma once
#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace StremioTranslate
{
    // One declared extra on a catalog, normalized from BOTH the modern `extra[]` objects and the legacy
    // `extraRequired`/`extraSupported` string arrays, so nothing downstream has to know which form was used.
    struct Extra
    {
        QString     name;              // "search" | "genre" | "skip" | addon-defined
        bool        isRequired = false;
        QStringList options;           // possible values; empty = free-form
        int         optionsLimit = 1;  // how many a user may select (schema default is 1)
    };

    // What MMV can actually do with a declared catalog.
    enum class CatalogUse
    {
        Browse,        // a shelf; any required extra can be satisfied from its options
        SearchOnly,    // requires `search` — answers queries, never a browse shelf
        Unsatisfiable  // requires something we cannot supply — skipped WITH A REASON
    };

    struct Catalog
    {
        QString                 type, id, name;
        QVector<Extra>          extras;
        CatalogUse              use = CatalogUse::Browse;
        QString                 skipReason;   // non-empty only when Unsatisfiable
        QMap<QString, QString>  presets;      // required extra -> its first option (may hold several)

        // "type/id", the form AddonManager already uses to route a Stremio catalog.
        QString routeId() const { return type + QLatin1Char('/') + id; }
    };

    struct Manifest
    {
        QString          id, name, version, description, logo;
        QStringList      types;
        QStringList      resources;   // resource NAMES, from string entries and object entries alike
        QStringList      idPrefixes;  // manifest-level
        // Per-resource overrides from the object form, keyed by resource name. A resource present here
        // uses ITS list; anything absent falls back to the manifest-level list.
        // Deliberate: an EXPLICITLY empty array ("idPrefixes": []) is recorded as absent, i.e. it falls
        // back to the manifest-level list rather than meaning "this resource accepts no ids". The Stremio
        // schema gives empty no distinct meaning, and reading it as "accepts nothing" would silently
        // unroute a resource over what is far more often a serializer artifact.
        QHash<QString, QStringList> resourceIdPrefixes;
        QHash<QString, QStringList> resourceTypes;
        QVector<Catalog> catalogs;
        bool             configurable = false;
        bool             configurationRequired = false;

        bool isValid() const { return !resources.isEmpty(); }
    };

    // Empty (isValid() == false) when the body is not a Stremio manifest.
    Manifest parseManifest(const QByteArray& body);

    // Past this many rows nobody is choosing. A parse/quota bound, NOT a display bound — NavMenu scrolls.
    constexpr int kMaxStreamRows = 30;

    // "/catalog/{type}/{id}/{k}={v}&{k}={v}.json". Per the protocol the extras are a URL-encoded query
    // string living in a PATH segment. Keys are emitted sorted so the same request always produces the same
    // string — AddonManager keys its result cache on it.
    //
    // `extras` is what the CALLER wants. The catalog's own presets are merged in ONLY where the caller gave
    // no value for that key, so a required `genre` defaults to its first option while a user picking
    // "Comedy" REPLACES that default instead of appearing alongside it.
    QString catalogPath(const Catalog& c, const QMap<QString, QString>& extras);

    // May this addon be asked for `resource` about `id`? Per-resource prefixes win over manifest-level;
    // an addon declaring no prefixes at all is always eligible.
    bool handlesId(const Manifest& m, const QString& resource, const QString& id);

    struct StreamCandidate
    {
        QString url, mime, infoHash;
        int     fileIdx = -1;
        QString name;        // the addon's short label — usually provider and/or quality
        QString title;       // the release line; addons commonly pack size and seeders in here
        QString bingeGroup;  // behaviorHints.bingeGroup — "keep using this source for the next episode"
        bool    notWebReady = false;
        qint64  videoSize = 0;
        int     seeders = -1;   // scraped out of `title` when present; -1 = unknown

        bool isDirect() const { return url.startsWith(QStringLiteral("http")); }
    };

    // Usable candidates only, sorted best-first and capped at kMaxStreamRows.
    QVector<StreamCandidate> parseStreams(const QByteArray& body);

    // One picker row: "1080p · Release.Name.x265 · 42 seeders · 2.1 GB".
    QString describe(const StreamCandidate& c);

    // The automatic choice: a candidate whose bingeGroup matches `preferGroup` (when non-empty), else the
    // first in sorted order. Returns -1 when there is nothing playable.
    int pickAuto(const QVector<StreamCandidate>& all, const QString& preferGroup);
}
