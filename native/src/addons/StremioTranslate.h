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
        QHash<QString, QStringList> resourceIdPrefixes;
        QHash<QString, QStringList> resourceTypes;
        QVector<Catalog> catalogs;
        bool             configurable = false;
        bool             configurationRequired = false;

        bool isValid() const { return !resources.isEmpty(); }
    };

    // Empty (isValid() == false) when the body is not a Stremio manifest.
    Manifest parseManifest(const QByteArray& body);
}
