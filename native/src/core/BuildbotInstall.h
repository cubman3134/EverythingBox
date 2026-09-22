// The "All cores" browser's impure half (issue #98, increments 2-3): fetch the buildbot index, fetch one core's
// zip, and turn it into a registered custom core — or, for a core installed that way before, replace it.
//
// THE PATH, IN ORDER:
//   1. Policy. Every URL is checked against the FetchPolicy's predicate BEFORE the request is made, and again
//      on every redirect (a redirect the predicate refuses aborts the transfer). The production policy's
//      predicate is BuildbotIndex::isAllowedUrl: https, the exact buildbot host, nothing else.
//   2. Download, capped: the index at BuildbotIndex::kMaxIndexBytes, a core zip at kMaxZipBytes. A transfer
//      that crosses the cap is aborted, not truncated.
//   3. Extract ONLY the entry's expected core file (extractCoreFromZip with an expected name — the same zip path
//      CoreManager uses for a catalogue core, whose basename-only output is the traversal refusal) into a
//      STAGING directory under <data>/cores/custom/.
//   4. Inspect the staged file (CoreInspect — load, retro_get_system_info, unload). A file that does not load
//      or does not answer is DELETED and the user is told why. Nothing is registered, and for an update the
//      old core is still the installed one.
//   5. Swap: the staged file replaces <data>/cores/custom/<core file> in one rename (replace-existing), so at
//      every instant the destination is either the whole old core or the whole new one. A swap that fails
//      (the old core is loaded by a running game, say) deletes the staged file and keeps the old one.
//   6. Register through CustomCores with the source recorded (buildbot, the index's file, the index date), so
//      the not-curated notice, the capability report and the per-system choice apply exactly as they do to a
//      hand-loaded core.
//
// THE TEST-ONLY OVERRIDE. effectivePolicy() is the production policy UNLESS the process runs under EB_UITEST
// AND EB_UITEST_BUILDBOT_BASE names a LOOPBACK http(s) base — then the base is that URL and the predicate
// admits exactly that scheme+host+port. Either condition missing (or a non-loopback override) and the override
// is ignored. It exists so a live drive can serve the index and the zips from its own stub; it cannot point a
// user's app at another host.
#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <functional>

#include "BuildbotIndex.h"
#include "CustomCores.h"

class QObject;

namespace BuildbotInstall
{
    constexpr qint64 kMaxZipBytes = 256LL * 1024 * 1024;   // the largest core zip on the buildbot is well under this

    struct FetchPolicy
    {
        QUrl base;                                  // the nightly root, ending in '/'
        std::function<bool(const QUrl&)> allow;     // THE decision, asked before every request and redirect
    };

    FetchPolicy productionPolicy();                 // productionBase() + BuildbotIndex::isAllowedUrl
    // PURE. The policy for a given environment: `uitest` = EB_UITEST is set, `overrideBase` =
    // EB_UITEST_BUILDBOT_BASE. Production unless BOTH are present and the override is a loopback http(s) URL.
    FetchPolicy policyFor(bool uitest, const QString& overrideBase);
    FetchPolicy effectivePolicy();                  // policyFor(the real environment)

    // Extract a core library from zip bytes into destDir. With an EMPTY `expectedFile` this is exactly the
    // catalogue path CoreManager always had: the first entry ending in `ext`, written under its BASENAME only
    // (a "../" or absolute path inside the zip cannot steer the write). With a non-empty `expectedFile` only an
    // entry whose basename IS that file is extracted — anything else in the archive is ignored, and an archive
    // without it fails. *outPath gets the written file.
    bool extractCoreFromZip(const QByteArray& zipData, const QString& destDir, const QString& ext,
                            const QString& expectedFile, QString* outPath);

    // Download `url` under `policy` with a byte cap. onDone(bytes, error) fires exactly once (error empty on
    // success) unless `context` is destroyed first, which aborts the transfer and drops the callback.
    void fetch(const FetchPolicy& policy, const QUrl& url, qint64 maxBytes, QObject* context,
               const std::function<void(int percent)>& onProgress,
               const std::function<void(const QByteArray& bytes, const QString& error)>& onDone);

    // Fetch and parse this platform's index.
    void fetchIndex(const FetchPolicy& policy, QObject* context,
                    const std::function<void(const BuildbotIndex::Parsed& parsed, const QString& error)>& onDone);

    // Steps 3-6 above on zip bytes already in hand (the probe drives this directly as well as through fetch).
    // Returns true with the registered record in *out; false with a sentence in *error, having left the
    // installed core (if any) and the registry untouched and the staging area empty.
    bool installFromZip(const QByteArray& zipData, const BuildbotIndex::Entry& entry, CustomCore* out,
                        QString* error);

    // The whole install/update: fetch the entry's zip under `policy`, then installFromZip. onDone(ok, record,
    // message) — the message is the reason on failure.
    void install(const FetchPolicy& policy, const BuildbotIndex::Entry& entry, QObject* context,
                 const std::function<void(int percent)>& onProgress,
                 const std::function<void(bool ok, const CustomCore& rec, const QString& error)>& onDone);

    // <data>/cores/custom/.staging — where a download waits while it is inspected.
    QString stagingDir();
}
