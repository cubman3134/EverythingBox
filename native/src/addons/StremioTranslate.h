// Translates the Stremio addon protocol into EB's models. Pure: JSON in, structs out — no network, no
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

    // What EB can actually do with a declared catalog.
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
    // `inline` so every translation unit shares ONE entity: a plain namespace-scope `constexpr` is
    // internally linked, giving each TU its own copy and its own address.
    inline constexpr int kMaxStreamRows = 30;

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

    // Which of `manifests` should be asked for `resource` about a `type`/`id` — indices, in input order.
    // Callers pass only the manifests of addons they already consider usable (e.g. enabled ones); this
    // knows nothing about installation state.
    //
    // Two stages. First the addons that OFFER `resource` for `type`; then, among those, the ones whose
    // idPrefixes claim this id space. THE SECOND STAGE MAY NEVER COST A RESULT: when it selects nobody,
    // every offering addon is returned instead and *fellBackToAll is set. A mis-declared or unusual manifest
    // must degrade to the old ask-everyone behaviour, never to an unplayable item — routing is an
    // optimization, and it is not allowed to be the reason nothing plays.
    QVector<int> routeProviders(const QVector<Manifest>& manifests, const QString& resource,
                                const QString& type, const QString& id, bool* fellBackToAll = nullptr);

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

    // Usable candidates only, sorted best-first and capped at `maxRows`.
    //
    // The cap is a PARAMETER, not a constant, because two callers want two different bounds out of the same
    // parse. The picker wants kMaxStreamRows — past 30 rows nobody is choosing. The debrid resolution path
    // wants as many as its batch cached-check can carry: it never shows these rows, it only asks TorBox
    // "which of these hashes do you already have", and a row it never parsed is a cached release it can
    // never play. Baking the picker's bound into the parse silently halved that pool for the single-addon
    // setup (one Torrentio response is routinely 50-100+ rows), turning "plays fine" into "nothing cached".
    // Same parse, two bounds — do not collapse them back together.
    QVector<StreamCandidate> parseStreams(const QByteArray& body, int maxRows = kMaxStreamRows);

    // The candidate order parseStreams applies: instant beats a debrid round-trip, then seeders, then size.
    // Exposed rather than only applied inside parseStreams because an AGGREGATE across several stream addons
    // has to be ordered by the SAME rule — concatenating per-addon blocks would rank one addon's worst row
    // above another's best, and auto-play would take a cold torrent over a neighbour's instant http url.
    // Stable, so an addon's own ordering survives among rows this cannot tell apart.
    void sortCandidates(QVector<StreamCandidate>& v);

    // Flatten several addons' (individually sorted) blocks into ONE list ordered by sortCandidates.
    // This exists as a named, testable function rather than a concat-then-sort at the call site because the
    // concatenation ALONE is a plausible-looking bug: each block is sorted, so the merged list reads as
    // sorted right up until addon B's instant http url sits below addon A's cold torrent and auto-play takes
    // the torrent. Not re-capped — the per-response bound already applied, and dropping rows here would only
    // shrink what the debrid cached-check can hit.
    QVector<StreamCandidate> mergeCandidates(const QVector<QVector<StreamCandidate>>& perAddon);

    // The one-row budget for describe(), in characters, ellipsis included. 96 is the width at which a row
    // still fits the picker's panel on one line at the smallest form factor we ship (the TV/handheld key
    // metrics), while still leaving room for the quality tag, the release name's distinguishing tokens and
    // the trailing size — i.e. everything a user actually chooses on. A raw-torrent addon happily returns
    // 180-character release lines; NavMenu word-wraps, so without a cap one candidate occupies two rows and
    // the list stops being scannable.
    inline constexpr int kMaxDescribeChars = 96;

    // One picker row: "1080p · Release.Name.x265 👤 42 · 2.1 GB". The seeder count is NEVER appended —
    // it was scraped out of the very title being rendered, so it is redundant by construction — and the
    // behaviorHints size is appended only when the title does not already carry one. The result is always
    // one line and never longer than kMaxDescribeChars (elided with … past that).
    QString describe(const StreamCandidate& c);

    // The automatic choice: a candidate whose bingeGroup matches `preferGroup` (when non-empty), else the
    // first in sorted order. Returns -1 when there is nothing playable.
    int pickAuto(const QVector<StreamCandidate>& all, const QString& preferGroup);
}
