// The libretro buildbot's core index, read — issue #98's "All cores" browser, its pure half.
//
// WHAT THE INDEX IS. Every platform directory on the nightly buildbot (the one CoreManager already downloads
// catalogue cores from) carries a plain-text `.index-extended` beside its zips, one core per line:
//
//     2024-03-01 6bf0c4c8 2048_libretro.dll.zip
//     <date>     <crc32>  <file>                      [an optional fourth field, a byte size, is accepted]
//
// This unit turns that body into entries (core name, file, date, crc, size where present) and classifies each
// against what the app already knows. It performs NO I/O: the bytes come in, a model comes out, and every
// decision the browser makes about an index — what is offered, what is refused, what counts as an update — is
// taken here where probe_buildbotindex pins it with literal fixtures.
//
// CAPS AND REFUSALS. A body over kMaxIndexBytes is refused whole (the real Windows index is tens of KiB; a
// body a hundred times that is not an index). At most kMaxEntries entries are kept; the rest are counted as
// dropped. A line that does not parse is SKIPPED AND COUNTED, never fatal — one bad line cannot cost the user
// the rest of the list. A line that parses but names a file this platform's core naming does not produce, or a
// core name outside the name rule (isValidCoreName: no path segment, no separator, no dot, no colon) is refused
// and counted separately: the name becomes a FILE NAME on disk and a URL path segment, so it is the one field
// that must never carry a path.
//
// THE THREE CLASSES. `catalogue` — the app already ships a way to get this core (SystemCatalog lists it), so
// the browser never offers it: it installs the normal way, through CoreManager. `installed-custom` — a custom
// core is registered whose file this entry produces (installed from here earlier, or loaded by hand from the
// same file). `available` — everything else.
//
// THE HOST RULE. isAllowedUrl() is THE decision about where the browser may fetch from: https, the exact
// buildbot host CoreManager already uses, the default port, no credentials in the URL. It is a pure function so
// it can be pinned without a network, and the fetch code takes it as an injected predicate — which is also how
// the probe drives the byte path against a loopback stub without the rule itself ever admitting loopback.
#pragma once
#include <QDate>
#include <QList>
#include <QSet>
#include <QString>
#include <QUrl>

#include "CustomCores.h"

namespace BuildbotIndex
{
    // ---- caps -------------------------------------------------------------------------------------------
    constexpr qint64 kMaxIndexBytes = 4 * 1024 * 1024;   // the index body
    constexpr int    kMaxEntries    = 2000;               // entries kept from one index

    // ---- the platform's buildbot naming (the ONE copy; CoreManager calls these) ----------------------------
    // "windows/x86_64/latest/" etc. — the directory under the nightly root this build's cores live in.
    QString subpath();
    // The shared-library suffix the buildbot ships for this platform (".dll" / ".dylib" / ".so").
    QString libExt();
    // "<core>_libretro<ext>" ("<core>_libretro_android.so" on Android): the core's file on disk, and the zip's
    // name minus ".zip".
    QString coreFileName(const QString& coreName);
    // The file-name tail every core of this platform carries ("_libretro.dll", "_libretro_android.so", …).
    QString coreFileTail();

    // ---- the name rule ------------------------------------------------------------------------------------
    // A core name the browser will accept: 1–64 characters of [A-Za-z0-9_+-], starting with a letter or digit.
    // No '/', '\\', '.', ':' or space can pass — so no name can be a path, a parent segment, a drive, a URL
    // scheme or a "custom:" ref.
    bool isValidCoreName(const QString& name);
    // "<name><tail>.zip" -> "<name>" when `zipFile` has exactly the shape `tail` gives (the platform's own is
    // coreFileTail()) and the name passes the rule; "" otherwise.
    QString coreNameFromZip(const QString& zipFile, const QString& tail);

    // ---- the parser ---------------------------------------------------------------------------------------
    struct Entry
    {
        QString name;          // "2048"
        QString file;          // "2048_libretro.dll.zip" — the index's own file field, verbatim
        QString coreFile;      // "2048_libretro.dll" — what gets extracted and installed
        QDate   date;          // the index date (always valid on a kept entry)
        QString crc;           // the index's crc field, lowercased ("" when it was "-")
        qint64  size = -1;     // bytes, when the line carried a fourth field; -1 otherwise
    };

    struct Parsed
    {
        QList<Entry> entries;       // in index order, at most kMaxEntries
        int  malformed = 0;         // lines that did not parse (wrong field count, bad date, bad size)
        int  refused = 0;           // lines that parsed but named a file/core the rule refuses
        int  dropped = 0;           // well-formed entries past kMaxEntries
        bool tooLarge = false;      // the body exceeded kMaxIndexBytes — nothing was parsed
    };

    // Parse with THIS platform's naming.
    Parsed parse(const QByteArray& body);
    // Parse against an explicit core-file tail (the probe's handle on a platform-independent fixture).
    Parsed parseWithTail(const QByteArray& body, const QString& tail);

    // ---- classification -----------------------------------------------------------------------------------
    enum class Status { Catalogue, InstalledCustom, Available };

    struct Row
    {
        Entry   entry;
        Status  status = Status::Available;
        QString customId;           // the registered custom core it matches (InstalledCustom only)
        bool    fromBuildbot = false;    // that custom core was installed from the buildbot (not loaded by hand)
        bool    updateAvailable = false; // ...and the index now carries a newer date for its file
    };

    // Every catalogue core base name (SystemCatalog's candidate lists, all systems).
    QSet<QString> catalogueCoreNames();

    // PURE. True only for a custom core installed from the buildbot, from THIS entry's file, whose recorded
    // index date is older than the entry's. A hand-loaded core (no source) never has an update.
    bool updateAvailable(const CustomCore& core, const Entry& e);

    // PURE. Classify every entry. Catalogue wins over everything (a catalogue core is never offered here).
    QList<Row> classify(const QList<Entry>& entries, const QSet<QString>& catalogue,
                        const QList<CustomCore>& customs);

    // PURE. What the browser lists for `query`: every non-catalogue row whose name contains the query
    // (case-insensitive, trimmed; an empty query matches everything), in index order.
    QList<Row> browsable(const QList<Row>& rows, const QString& query);

    // ---- where the browser may fetch from ------------------------------------------------------------------
    QString host();                 // "buildbot.libretro.com"
    QUrl    productionBase();       // https://buildbot.libretro.com/nightly/
    // THE HOST RULE: https, exactly host(), default port, no user info. Everything else is refused.
    bool    isAllowedUrl(const QUrl& url);
    QUrl    indexUrl(const QUrl& base);                       // <base><subpath>.index-extended
    QUrl    zipUrl(const QUrl& base, const Entry& e);         // <base><subpath><file>
    // CoreManager's catalogue-core zip URL, exactly the one it always built. EMPTY for a "custom:" ref: a custom
    // core is never fetched by name — the rule CoreManager's two fetch paths keep, spelled here a second time so
    // the URL builder itself cannot produce one. (The browser's name rule is NOT applied here: catalogue names
    // come from SystemCatalog, and nothing about how a catalogue core installs changes in #98.)
    QString catalogueZipUrl(const QString& coreName);
}
