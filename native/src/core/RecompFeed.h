// THE RETCOMM CATALOGUE AS A SECOND FEED (issue #248, increment b).
//
// WHAT IT IS. RetComM Launcher publishes its title catalogue as a build artefact rather than as part of its
// program: `catalog.zip` on the latest GitHub release of `TechnicallyComputers/retcomm-catalog`, holding
// `index.json` (a list of title ids, a `release_tag` and a `catalog_date`, plus per-platform BIOS defaults
// this build does not read) and one `titles/<id>.json` per entry in the schema #233 already writes our own
// in-tree catalogue in. Its own README states the contract in as many words: the launcher "checks that
// release identity / stamp on startup and downloads the zip only when the remote catalog is newer".
//
// WHY IT IS A SECOND FEED AND NOT A REPLACEMENT. Our in-tree catalogue carries knowledge the published one
// does not and cannot: `rom_delivery` (how a port takes the game file — our extension, because the schema has
// no field for it) and a `license` we checked. The published one carries fifteen-odd PSX titles we do not
// track. So they are MERGED BY TITLE IDENTITY and the in-tree entry WINS: same console, same game, our row.
// Merging on the id would not do it — ids are each project's own slug ("zelda64recomp" against
// "twisted-metal4-psx"), and two catalogues describing one game agree on the game, never on the slug.
//
// WHAT A FEED-ONLY ROW MAY DO, WHICH IS ALMOST NOTHING YET. Every entry in the published catalogue names a
// build engine, so every one of them is the SELF-COMPILED tier — the port is produced on this machine from a
// recompiler plus the user's own dump. This increment does not compile anything (that is increment (c)); a
// self-compiled row therefore lists, states its engine and licence, and its Install says that building here
// arrives later and offers the engine's own page. An Install that silently could not happen would be worse
// than the sentence.
//
// LICENCE IS THE ENGINE'S. A published title manifest has no licence field — the licence that governs a
// self-compiled port is the recompiler's, and all three named engines are PolyForm Noncommercial 1.0.0. That
// is a licence a person is entitled to read BEFORE they ask for a build, so it is on the row. Nothing of any
// engine is bundled in this app, and this increment fetches none of them: the only bytes it downloads are the
// catalogue's own JSON.
//
// FAILURE POSTURE (#174, and it is the reason this file has a `shapeError` rather than an empty list). A feed
// that cannot be reached keeps the LAST GOOD COPY on disk and says nothing; a document that cannot be parsed
// is an ERROR ROW in the section, never an empty section — and it does not overwrite the good copy, because
// a publisher who ships one broken build should not thereby delete the working catalogue on every machine.
//
// SPLIT: everything above `refresh()` is QtCore-only (miniz for the zip, which is portable C), so probe_ports
// drives the parse, the merge and the cache headlessly off a fixture zip it builds in memory. `refresh()` is
// the one network-touching function and lives in its own translation unit.
#pragma once
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include "EmulatorRegistry.h"

namespace RecompFeed
{
    // The published artefact. Its own README names this exact URL as the one the launcher fetches; it is a
    // `releases/latest/download/` alias, so it redirects and needs a redirect-following fetch.
    //
    // Overridable through EB_RECOMM_CATALOG_URL, and that is a TEST seam with a real user behind it too: a
    // machine behind a mirror, and every live drive of this feature, must be able to point it somewhere that
    // is not the real repository. Nothing else in this app may name the URL.
    QString catalogUrl();

    // <data>/recomps — the last good copy's home. Its own folder rather than <data>/ports, which is the
    // USER's override directory: a file the app rewrites on a schedule does not belong among files a person
    // hand-edits, and NativePorts::loadPortsDir reads every *.json in there.
    QString cacheDir();
    QString cachedCatalogPath();   // <data>/recomps/catalog.zip

    // ---- the recompiler engines, by name ---------------------------------------------------------------
    // A tiny hard table rather than catalogue data, because it is the one thing on the row that must not come
    // from the feed: a licence read out of a document the licensor does not control is not a licence notice.
    // All three are PolyForm Noncommercial 1.0.0 — checked against each project's own LICENSE file.
    struct Engine
    {
        QString id;        // "psxrecomp" / "snesrecomp" / "gbarecomp" — the catalogue's build.generate.engine
        QString license;   // "" for an engine this build has not checked; a guess would be worse than silence
        QString homepage;  // where the Install verb sends somebody who wants it today
    };
    Engine engineInfo(const QString& engineId);

    // ---- the parse (pure) ------------------------------------------------------------------------------
    struct Feed
    {
        QList<ExternalEmulator> titles;
        QString releaseTag;    // index.json's `release_tag` — the identity RetComM itself compares on
        QString catalogDate;   // index.json's `catalog_date`, UTC ISO-8601
        // Empty when the document was one this reader understands, INCLUDING when it understood it and it
        // held nothing. Non-empty when it was not — and then it is the sentence the error row shows. Same
        // shape, and the same reason, as ThemeRegistry::Index.
        QString shapeError;
        bool ok() const { return shapeError.isEmpty(); }
    };

    // Read an already-unpacked catalogue: member path -> bytes. Split out from the zip so the probe can drive
    // every shape failure without having to express it as a zip.
    Feed parseMembers(const QHash<QString, QByteArray>& members);

    // Unpack `catalog.zip` in memory. Empty map (with *error) on anything that is not a readable zip.
    QHash<QString, QByteArray> unpack(const QByteArray& zipBytes, QString* error = nullptr);

    // The two together: bytes in, Feed out. A zip that will not open is a shapeError like any other.
    Feed parseCatalogZip(const QByteArray& zipBytes);

    // ---- the merge (pure) ------------------------------------------------------------------------------
    // In-tree wins. A feed entry is dropped when an in-tree entry shares its id, or shares its platform AND
    // any spelling of its game's title (NativePorts::titleKeys, the same key the ROM match is made on — so
    // "the catalogue thinks these are the same game" and "the library thinks this dump is that game" can
    // never be answered by two different comparisons).
    QList<ExternalEmulator> mergeByTitleIdentity(const QList<ExternalEmulator>& inTree,
                                                 const QList<ExternalEmulator>& feed);

    // ---- the cached copy -------------------------------------------------------------------------------
    // The last good copy, parsed. shapeError set (and titles empty) when there has never been one.
    Feed cached();

    // Parse `zipBytes` and, ONLY if it parses, replace the cached copy. Returns the parse result either way:
    // a broken publish leaves the working catalogue on disk untouched and surfaces as an error row.
    Feed storeIfParses(const QByteArray& zipBytes);

    // ---- what the section browses ----------------------------------------------------------------------
    // The in-tree catalogue with the feed merged in. `feedError`, when given, receives the reason the feed
    // contributed nothing — empty when it contributed, or when there is simply no cached copy yet and no
    // refresh has failed.
    QList<ExternalEmulator> catalogue(QString* feedError = nullptr);

    // Resolve one row's id across BOTH sources. Returned BY VALUE: the feed's list is replaced wholesale on a
    // refresh, and a card that holds a pointer into it runs a nested event loop (NavConfirm) during which
    // exactly that can happen.
    bool findById(const QString& id, ExternalEmulator* out);

    // ---- the refresh schedule --------------------------------------------------------------------------
    // Once a day, and recorded in the portable ini beside every other such stamp. The catalogue is republished
    // when a submission is approved; a person who opens the section twice in an afternoon does not need two
    // fetches, and a person who never opens it needs none at all.
    bool dueForRefresh();
    void markRefreshed();
    void forgetRefreshStamp();   // tests + "check now"

    // ---- the one network-touching function (RecompFeedFetch.cpp) ---------------------------------------
    // BLOCKING. Fetches catalogUrl() under a hard byte ceiling and a deadline, hands the bytes to
    // storeIfParses, and returns the outcome. Call it from a worker thread; it is never correct on the GUI
    // one. A failed fetch returns a Feed whose shapeError names the failure and leaves the cached copy alone.
    Feed refresh();

    // The ceiling. The real artefact is ~28 KB; two orders of magnitude of headroom is generous for a
    // document of a few hundred JSON files, and small enough that a hostile release cannot make this a
    // download. Named so the probe and the fetch cannot disagree.
    inline constexpr qint64 kMaxCatalogBytes = 4 * 1024 * 1024;
    inline constexpr int    kFetchTimeoutMs  = 20000;   // registryFetchToBuffer's deadline, for parity
}
