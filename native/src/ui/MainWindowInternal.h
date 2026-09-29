// MainWindow's FILE-SCOPE helpers that more than one MainWindow translation unit needs (issue #186).
//
// MainWindow.cpp is being split into focused translation units behind the same class. A member function
// that moves out takes with it every file-static only IT uses. A file-static the rest of MainWindow.cpp
// ALSO uses must not be copied: two copies drift apart, and a merge that edits one of them compiles on one
// side and not the other. So each such helper is defined ONCE, here, and every MainWindow TU that needs it
// includes this header. Internal to src/ui/MainWindow*.cpp; nothing else should include it.
//
// These were `static` in MainWindow.cpp. In a header they are `inline`, so there is still exactly ONE
// function -- and ONE QSettings object behind store() -- however many TUs include this. `inline` means
// external linkage, though, and ~25 other files under src/ define their own `QSettings& store()`: almost all
// in anonymous namespaces, but a global one with the same signature would be silently merged with this one
// by the linker. The named namespace keeps the symbol unique; the using-declarations below keep every call
// site spelled exactly as it was.
#pragma once

#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QPushButton>
#include <QSettings>
#include <QString>

#include "../addons/AddonModels.h"
#include "../core/AppBrand.h"
#include "../core/AppPaths.h"
#include "../core/LogSafeText.h"
#include "../core/RecentStore.h"
#include "../core/RomLibrary.h"
#include "../core/RomhackClient.h"
#include "../core/StoredUrl.h"

namespace mainwindow_internal {

// Per-profile settings store (resume positions, etc.), mirroring the accessor the other views use.
inline QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// A large, left-aligned menu row for the inline settings pages (TV/remote-friendly target size).
inline QPushButton* panelRow(const QString& label)
{
    auto* b = new QPushButton(label);
    b->setMinimumHeight(54);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral(
        "QPushButton{font-size:17px;text-align:left;padding:14px 20px;border:1px solid rgba(0,0,0,0.12);"
        "border-radius:10px;background:rgba(0,0,0,0.04);} QPushButton:hover{background:rgba(0,0,0,0.10);}"
        "QPushButton:focus{border:2px solid #2C72C9;}"));
    return b;
}

// One-line append to <app>/stream_debug.log, shared with the addon stream/manga resolution tracing.
inline void mwLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg + QStringLiteral("\n")).toUtf8());
}

// A log-safe rendering of a URL: scheme://host[:port]/…/<filename>. Drops the path's middle segments (which
// can carry an addon access token) and the query string (which can carry debrid keys), so logs never leak secrets.
// THE RULE ITSELF now lives in core/LogSafeText.h — it was written out identically here, in MainWindow.cpp,
// DownloadManager.cpp and StreamResolver.cpp, and #231 needed a fourth caller. Same rendering, one definition.
inline QString logSafeUrl(const QString& url) { return LogSafeText::url(url); }

// Whether `id` may be written into RecentItem::sourceItemId — i.e. whether it is an ID rather than a LINK.
//
// !!! THE #200 HOLE THIS EXISTS TO KEEP SHUT. sourceItemId is written to everythingbox.ini in cleartext and
// rides the cross-device sync document, and RecentStore's scrubbed() deliberately does NOT clean the four
// recipe fields — they are ids by construction, so there is no query to take off and location() could only
// corrupt an id containing a '?'. But MediaItem::id is not guaranteed to be id-shaped: for a keyless catalog
// stream the video leaf below records `rkey = item.id.isEmpty() ? url : item.id`, so on those sources item.id
// IS the signed url, token and all. Copying it into the recipe would put that token straight back into a
// synced field. See the warning block above RecentStore.cpp's scrubbed().
//
// THE PREDICATE IS StoredUrl::isNetworkUrl AND SPECIFICALLY NOT carriesCredential. carriesCredential(s) is
// exactly `location(s) != s` — true only when there is a query, userinfo or fragment TO REMOVE — so a bare
// "https://host/x.mkv" passes it, and a host that signs in the PATH (which StoredUrl states it deliberately
// does not reach) would pass it while carrying a credential. The question here is not "does this url carry a
// query", it is "is this a url at all", and isNetworkUrl answers exactly that over every scheme a credential
// can ride. A "meta:<blob>" release id and a "tt0111161" have no "://" and are unaffected.
//
// Refusing to write the recipe costs nothing. A url is not something a source can look up, so a url-shaped id
// was never re-mintable; the row simply falls back to replaying its path, which is every row's behaviour
// today and the behaviour that predates #224.
//
// WHY isNetworkUrl ALONE IS NOT THE WHOLE TEST. It only recognises a value with "://" and an allow-listed
// scheme before it, and four link shapes carrying a credential have neither. None is reachable from a value
// THIS tree mints, but both `item.id` and `item.imdbStreamId` are read verbatim out of an addon's JSON
// (AddonModels.cpp:246), so they are an addon's promise, not an invariant, and the field they land in syncs
// across devices in cleartext (#200). So the shapes are refused outright:
//
//   "//host/x.mkv?token=abc"                          scheme-relative — no "://" at all
//   "magnet:?xt=urn:btih:…&tr=https://tr/<passkey>/…" a private-tracker passkey in a query
//   " https://host/x?token=…"                         one leading space and scheme() returns empty
//   "webdav://u:p@host/x"                             a scheme outside the allow-list
//
// VERIFIED THAT NO LEGITIMATE ID IS REFUSED, rather than assumed. A "meta:<blob>" release id is the engine's
// EncodeId (EverythingBoxServer/EverythingBox.Server/Sources/IndexerSearchSource.cs): base64 with '=' trimmed
// and '+'→'-', '/'→'_', i.e. the alphabet [A-Za-z0-9-_] behind a "meta:" prefix — no '?', no '#', no '/', no
// space, never empty. An IMDB id ("tt0111161") and an episode id ("ttShow:1:2") are letters, digits and
// colons. A Stremio/addon item id is a path-free token (a single '/' would still pass; only "//" is refused).
// So the guard costs nothing real — and the cost of being wrong the other way is a live credential written
// into a synced ini.
inline bool remintableId(const QString& id)
{
    const QString trimmed = id.trimmed();
    if (trimmed.isEmpty() || trimmed != id) return false;     // empty, or padded so scheme() cannot see it
    if (id.contains(QLatin1Char('?')) || id.contains(QLatin1Char('#'))) return false;  // a query is not an id
    if (id.contains(QLatin1String("//"))) return false;       // "//host/…" and every "<scheme>://…"
    // Subsumed by the "//" test above and kept anyway: this is the rule the comment block argues for, and a
    // future relaxation of the shape tests must not silently take the url test out with them.
    return !StoredUrl::isNetworkUrl(id);
}

// The #224 re-mint recipe for a playable that a source just resolved. One spelling for all three
// RecentStore::add sites below, because the failure mode of three copies is that two get updated.
//
// The route is decided by what the item HAS, not by what resolved it: an item carrying an imdbStreamId can
// be re-resolved across every installed stream provider, which survives the addon that served it being
// uninstalled. A file-provider item without one can only be re-asked of the addon that knows its id space,
// so that route names the addon. `sourceAddonId` is recorded on BOTH routes — the imdb route ignores it,
// but it costs one short string and it is the only record of which addon actually served this play.
//
// EVERY FIELD OR NONE. When no route qualifies, all four are left empty rather than partly filled: the row
// then reads as a pre-#224 row and RecentStore::reopenFor sends it to ReplayPath, which is today's behaviour.
inline void applyRemintRecipe(RecentItem& row, const MediaItem& item)
{
    // A RECIPE ONLY MEANS ANYTHING FOR A ROW WHOSE PATH IS A NETWORK STREAM. The whole premise of #224 is
    // that the stored path is a link that expires; a file on disk does not expire, and re-opening it must
    // stay a local open that works with the network unplugged.
    //
    // THIS GUARD IS NOT REDUNDANT WITH THE ROUTE TESTS BELOW — do not remove it as tidy-up. Local-library
    // tiles carry an imdbStreamId (SyntheticCatalogs.cpp:99,101, added so subtitle matching has an exact
    // key), and BOTH prefer-local routes keep it while re-pointing the url at the file: HomeView's
    // resolvePlay (:7009-7018, url = the local path, mime "local:video") and MainWindow::playResolvedEpisode
    // for an owned next episode. Without this line those rows take the imdb branch, and reopenFor — which
    // has no path-exists preference — would send an owned movie on a debrid round trip that fails offline.
    //
    // isNetworkUrl is exactly the right predicate: false for a drive path, a UNC path and "file://", true for
    // the signed http(s)/rtsp/… links this feature exists for. `row.path` is set by the aggregate initialiser
    // at all three call sites before this runs.
    if (!StoredUrl::isNetworkUrl(row.path)) return;   // local playback: nothing to re-mint

    // imdbStreamId is guarded by the same rule as item.id and for a weaker but real reason: it is not always
    // minted here — AddonModels reads it straight out of an addon's JSON — so "it is an imdb id" is an addon's
    // promise, not an invariant. A url-shaped one is not an imdb id at all, so the item is treated as not
    // carrying one and the direct route below (guarded in its own right) still gets its chance.
    if (remintableId(item.imdbStreamId))
    {
        row.sourceAddonId = item.sourceAddonId;
        row.sourceRoute   = QStringLiteral("imdb");
        row.sourceItemId  = item.imdbStreamId;
        // resolveStreamByImdb takes the STREMIO type ("movie"/"series"), which is what an episode's parent
        // is; item.type on an episode leaf is "episode", which that call does not accept.
        row.sourceType = item.imdbStreamId.contains(QLatin1Char(':')) ? QStringLiteral("series")
                                                                      : QStringLiteral("movie");
        return;
    }
    // ALL THREE OF THE DIRECT ROUTE'S FIELDS, or none — which is what the EVERY FIELD OR NONE paragraph above
    // promises, and `item.type` is one of them: ResolveDirect hands it on as the re-asked MediaItem's own type
    // and reopenFor refuses a typeless recipe. Writing a three-field row would be absorbed downstream (that
    // refusal sends it to ReplayPath, so nothing leaks and nothing crashes) and would still be a row whose
    // stored shape contradicts the rule stated here — the exact drift this comment exists to prevent.
    // THE ADDON THAT CAN MINT A LINK, AND THE ID IT KNOWS THIS BY — which are not always the ones on the
    // item. A doc-bridge play (a title pressed on a metadata shelf, then searched for by name on a file
    // provider) reaches this with `sourceAddonId` naming the CATALOG and `id` in the catalog's space, and a
    // recipe built from those was un-re-mintable by construction: the addon it named has no /stream at all,
    // so re-opening the row failed instantly with nothing to say. remintAddonId/remintItemId carry the
    // provider and its release id when the two identities differ; everywhere else they are empty and this
    // reads exactly as it did. See AddonModels.h for why the item's own id stays the catalog's.
    const QString mintAddon = item.remintAddonId.isEmpty() ? item.sourceAddonId : item.remintAddonId;
    const QString mintId    = item.remintItemId.isEmpty()  ? item.id            : item.remintItemId;
    // BOTH OR NEITHER, checked on the pair actually being written. An override that named an addon but no id
    // (or the reverse) would otherwise mix half of one identity with half of the other and produce a recipe
    // that asks the file provider for a "googlebooks:…" id — a shape neither route would ever have written,
    // and one that fails at the provider instead of at the guard.
    if (item.remintAddonId.isEmpty() != item.remintItemId.isEmpty())
        return; // half an override is not an identity: the row replays its path
    if (mintAddon.isEmpty() || item.type.isEmpty() || !remintableId(mintId))
        return; // no recipe: the row replays its path
    row.sourceAddonId = mintAddon;
    row.sourceRoute   = QStringLiteral("direct");
    row.sourceItemId  = mintId;
    row.sourceType    = item.type;
}

// Where an installed hack belongs: the ROM LIBRARY's own folder for this system, never "beside the base ROM".
// A game that has been launched once carries the path of its EXTRACTED TEMP copy, so targeting the base ROM's
// directory quietly wrote the hack into the temp ROM folder, where the library never looks and a cleanup would
// delete it. Empty when no ROMs folder is set, which the callers report rather than guessing a path.
//
// folderFor() returns the folder NAME ("nes"), not a path — joining it to the library root is what makes it
// absolute. Passing it alone resolved against the process's working directory and quietly created a stray
// <cwd>/nes/ that the library never scans.
inline QString romLibraryFolderFor(const QString& systemId)
{
    if (RomLibrary::root().isEmpty()) return QString();
    return RomLibrary::root() + QLatin1Char('/') + RomLibrary::folderFor(systemId);
}

// Where a downloaded patch waits between arriving and being applied. Under downloads/ because that is where
// the manager's ".part" siblings already live, and because a patch that came through the queue and one that
// came down inline must land in the SAME place — the retry path cannot care which way it arrived.
inline QString romhackPatchCacheDir()
{
    return AppPaths::dataDir() + QStringLiteral("/downloads/patches");
}

// How to name the dump a patch wants, in a sentence someone can act on. The catalogued filename is the most
// useful form when there is one — it names the release the way a ROM site does.
inline QString describeTarget(const RomhackTarget& target)
{
    if (!target.fileName.isEmpty()) return target.fileName;
    if (!target.region.isEmpty())   return QObject::tr("the %1 release").arg(target.region);
    return QString();
}

} // namespace mainwindow_internal

using mainwindow_internal::store;
using mainwindow_internal::panelRow;
using mainwindow_internal::mwLog;
using mainwindow_internal::logSafeUrl;
using mainwindow_internal::remintableId;
using mainwindow_internal::applyRemintRecipe;
using mainwindow_internal::romLibraryFolderFor;
using mainwindow_internal::romhackPatchCacheDir;
using mainwindow_internal::describeTarget;
