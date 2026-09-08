// Custom (user-supplied) libretro cores — issue #98's escape hatch: run a core the catalogue does not bless.
//
// WHY A SEPARATE TIER, AND NOT A CATALOGUE ENTRY. SystemCatalog is the curated table: which systems exist, and
// which cores the app is willing to pick FOR you. A core the user hands us is a different kind of fact — it is
// not curated, it may be a fork, a build newer than our table, a core we deliberately excluded (Mesen), or not
// an emulator at all (2048, mrboom). So it is registered BESIDE the catalogue's candidates, never inside them:
//
//   * augmentCandidates() APPENDS matching custom cores to a system's candidate list, in registration order,
//     after every catalogue core. cores[0] — the system's DEFAULT — is therefore untouched by construction, so
//     a custom core can only ever run when it is EXPLICITLY chosen (per system through the default-core picker,
//     per game through #51's override, whose resolveCore accepts exactly the candidates listed here).
//   * With NO custom cores registered, augmentCandidates returns the catalogue list ITSELF — byte-for-byte,
//     same order, same values. That is the no-custom-core identity rail probe_customcore pins, the same posture
//     #100's N=0 and #92's no-data-files rails take: nothing about the curated path changes until the user
//     deliberately adds something.
//
// THE REF. A custom core is named "custom:<id>" everywhere a catalogue core is named by its base name. The
// prefix is load-bearing three times over: it can never collide with a buildbot core name (none contains a
// colon), it tells CoreManager to resolve the path from THIS registry instead of building a buildbot URL — so a
// custom core is never downloaded, only ever loaded from the file the user named — and it makes every log line
// that carries a core name self-labelling, which is the "crashes are labelled as coming from a custom core"
// half of the warranty below.
//
// THE WARRANTY, STATED ONCE. `noticeDue()` is true until the user has seen noticeText() once; the UI shows it
// on the first successful load and calls acknowledgeNotice(). It is never shown again, and a known-bad core is
// never REFUSED — the entire point of the feature is that the user overrides us, so the posture is advisory.
//
// SUPPORTS_NO_GAME. A core that declares RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME runs with no content at all
// (the game-engine cores). It has no ROM to be launched from, so runEntries() lists exactly those cores: they
// get a direct Run entry, and a content-requiring core gets none (it is reached by opening a game instead).
//
// STORAGE. <data>/cores/custom/registry.json — one small JSON document, QtCore-only, with the same
// malformed-is-logged-and-skipped posture SystemCatalog's data dir has: a registry that will not parse yields
// an EMPTY registry (never a crash, never a partially-applied one), and the curated path carries on untouched.
// The core FILES themselves are copied into <data>/cores/custom/ on load, so a core keeps working after the
// original download folder is cleared — and a file the user drops in that folder by hand is picked up by the
// same scan.
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// One registered custom core. `path` is device-local (it names a file on THIS machine), which is why the whole
// registry is device-local and does not ride the settings sync: a peer that does not have the file would carry
// a candidate it can never load.
struct CustomCore
{
    QString     id;           // stable, file-safe key derived from the core's own library_name
    QString     path;         // absolute path to the core library on this device
    QString     name;         // retro_get_system_info().library_name, verbatim
    QString     version;      // retro_get_system_info().library_version, verbatim ("" when the core gives none)
    QStringList extensions;   // valid_extensions, lowercased and split (no leading dots); may be empty
    bool        supportsNoGame = false; // declared RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME -> gets a Run entry
    bool        needFullpath = false;   // retro_get_system_info().need_fullpath
    QString     needs;        // "" = nothing missing; else the sentence naming what this core asked us for
    qint64      addedAt = 0;  // epoch ms, registration order tiebreak (the list order is authoritative)
};

inline bool operator==(const CustomCore& a, const CustomCore& b)
{
    return a.id == b.id && a.path == b.path && a.name == b.name && a.version == b.version
        && a.extensions == b.extensions && a.supportsNoGame == b.supportsNoGame
        && a.needFullpath == b.needFullpath && a.needs == b.needs && a.addedAt == b.addedAt;
}
inline bool operator!=(const CustomCore& a, const CustomCore& b) { return !(a == b); }

// The whole on-disk document: the acknowledgement flag for the one-time notice, plus the cores in
// registration order. A husk-free store — this is device-local intent about files on THIS machine, so a
// removal is a real removal and there is no peer to resurrect it.
struct CustomCoreRegistry
{
    bool             noticeAcknowledged = false;
    QList<CustomCore> cores;
};

namespace CustomCores
{
    // ---- pure vocabulary: the "custom:<id>" ref ----------------------------------------------------------
    QString refFor(const QString& id);          // "custom:<id>" ("" for an empty id)
    bool    isCustomRef(const QString& ref);    // does this core name belong to the custom tier?
    QString idFromRef(const QString& ref);      // "<id>" for a custom ref, "" otherwise
    // A core's own library_name reduced to a file-safe, stable key: lowercased, every run of characters
    // outside [a-z0-9] collapsed to one '_', leading/trailing '_' trimmed. Empty in, empty out — the caller
    // falls back to the file's base name, and if THAT is empty too the load is refused (an id is required).
    QString sanitizeId(const QString& raw);

    // ---- pure: CustomCore <-> canonical JSON ------------------------------------------------------------
    // Canonical: id/path/name always written, every other field only when non-default, so there is exactly one
    // spelling per record and fromJson(toJson(c)) == c.
    QJsonObject toJson(const CustomCore& c);
    CustomCore  fromJson(const QJsonObject& o);

    // ---- pure: the whole document <-> bytes -------------------------------------------------------------
    QByteArray         serialize(const CustomCoreRegistry& r);
    // Bytes that are not a JSON object, or whose "cores" is not an array, yield an EMPTY registry and a reason
    // in *err — never a partial parse. An entry with no usable id or no path is skipped (with a reason), so one
    // bad row cannot cost the user the rest of their cores.
    CustomCoreRegistry parse(const QByteArray& bytes, QString* err);

    // ---- THE PURE MODEL ----------------------------------------------------------------------------------
    // The candidate cores for a system: `catalogue` unchanged and in order, then every custom core that claims
    // one of `systemExtensions`, in registration order, as "custom:<id>". Never reorders, never removes, never
    // inserts before a catalogue core — so cores[0] stays the catalogue default and a custom core is reachable
    // only by an explicit choice. Duplicates are dropped (a ref already present is not appended twice). With an
    // empty `customs` the result IS `catalogue` — the identity rail.
    QStringList augmentCandidates(const QStringList& catalogue, const QStringList& systemExtensions,
                                  const QList<CustomCore>& customs);

    // PURE. May the frontend FETCH this core when it is not installed? False for a custom ref and true for
    // every catalogue core. The policy behind CoreManager's two download paths, spelled here so it is pinned
    // by a probe rather than only by reading the call sites: #98 loads a file the user already has, and the
    // moment a custom ref could reach a buildbot URL the feature would have quietly grown a download it was
    // explicitly scoped not to have (the "All cores" browser is the increment after this one).
    bool mayDownload(const QString& coreName);

    // The cores that earn a direct Run entry: exactly those declaring supports_no_game. A content-requiring
    // core has no Run entry — it is reached by opening a game whose extension it claims. Order preserved.
    QList<CustomCore> runEntries(const QList<CustomCore>& customs);

    // ---- the one-time warranty notice --------------------------------------------------------------------
    bool    noticeDue(const CustomCoreRegistry& r); // true until acknowledged; false forever after
    QString noticeText();                           // the sentence; stated once, never nagged

    // ---- the registry (cached; <data>/cores/custom/registry.json) -----------------------------------------
    QString customDir();     // <data>/cores/custom (created on demand)
    QString registryPath();  // <data>/cores/custom/registry.json

    const CustomCoreRegistry& registry();   // parsed once, then cached
    void reload();                          // re-read from disk (the probes and the loader use this)
    const QList<CustomCore>& all();         // registry().cores

    const CustomCore* byRef(const QString& ref);      // nullptr when not a registered custom ref
    QString pathForRef(const QString& ref);           // the file path, "" when unknown
    // "<name> (custom core)" for a registered ref; "" for anything else. The tag is deliberate: a user looking
    // at a picker row must be able to see, without clicking, which rows the app does not stand behind.
    QString displayNameFor(const QString& ref);

    // Register (or re-register) a core. A record whose id already exists is REPLACED — re-loading the same
    // core after rebuilding it is an update, not a duplicate. Writes the registry; false + *err on failure.
    bool add(const CustomCore& c, QString* err = nullptr);
    bool remove(const QString& id);          // true if a record was removed
    void acknowledgeNotice();                // the notice has been shown; never show it again
}
