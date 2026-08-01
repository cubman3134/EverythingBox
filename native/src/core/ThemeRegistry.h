// ThemeRegistry — the pure core of the in-app theme gallery: what a community registry index means, and
// what may be written to disk because of one.
//
// A themes2 theme is a FOLDER (theme.json plus optional sounds/ and fonts/), and a registry entry names
// that folder rather than listing files: {name, author, description, dir: "themes2/<Name>"}. The dead
// RegistryBrowser::Themes path this replaces read a "file"/"assets" pair that no live entry has ever had.
//
// Everything here is QtCore-only and network-free so probe_themereg can pin it headlessly, and so BOTH
// Appearance builders share one copy of the parts that are easy to get subtly wrong: which index key to
// read, which paths may become filenames, and how a folder lands on disk without a half-install surviving.
#pragma once
#include <QByteArray>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ThemeRegistry {

// Refuse an entry whose listing is implausible for a theme. A registry is public and anyone may open a
// pull request against one; these are the bounds beyond which we stop rather than download.
inline constexpr int    kMaxFiles     = 64;
inline constexpr qint64 kMaxFileBytes = 8 * 1024 * 1024;

// The whole entry's downloaded bytes, and the reason the per-file cap is not enough on its own. kMaxFiles
// files of kMaxFileBytes each is 512 MB, and the download loop holds every one of them in memory as a
// QByteArray until the last file lands (that is what makes the install atomic) — so the per-file cap alone
// buys an attacker a half-gigabyte allocation on a product that ships to a 32-bit armv7 Android TV box,
// where the whole process address space is 2 GB minus Qt's own footprint. 32 MB caps the peak at something
// that box can hold while leaving an order of magnitude of headroom over any real theme: the three bundled
// themes are well under 1 MB each, and a lavish one (a couple of fonts, a handful of sounds, a wallpaper)
// is a few MB. A theme that genuinely needs more than this is installed by hand.
inline constexpr qint64 kMaxTotalBytes = 32 * 1024 * 1024;

struct Entry {
    QString     name;          // display text ONLY — never used as a path
    QString     author;
    QString     description;
    QString     dir;           // "themes2/<Name>", relative to the index URL's directory
    QStringList formFactors;   // advisory note on the row; does not filter

    // The install folder: the last segment of `dir`. Empty when `dir` is unusable, which is the single
    // predicate callers check — parseIndex already drops those, so an Entry in hand always has one.
    QString folder() const;
};

// Parse a registry index. Reads "themes2" (what the registry serves) and falls back to "themes" (the key
// the pre-existing code assumed) ONLY when "themes2" is absent — a present themes2 wins outright, even
// empty or malformed, so a registry that empties it is not silently answered from the legacy list.
// Entries without a usable `dir` are DROPPED, so every returned Entry has a non-empty folder().
QVector<Entry> parseIndex(const QByteArray& json);

// May this relative path become a filename? Accepts only a relative path whose every segment is plain:
// no "." or ".." segment, no leading "/", no drive letter, no backslash, no empty segment, no Windows
// reserved device name, no Windows-illegal character (< > : " | ? * and control characters), and no
// trailing "." or " " — Win32 strips those before resolving, so such a segment names something other than
// itself on disk. Rejects rather than sanitises: rewriting a hostile path guesses at intent, and no theme
// has a benign reason to ship one. No path in, no path out — every answer here is a plain yes or no.
bool isSafeRelPath(const QString& rel);

// raw.githubusercontent.com/<owner>/<repo>/<branch>/index.json
//   -> api.github.com/repos/<owner>/<repo>/git/trees/<branch>?recursive=1
// Empty for any other host or a URL too short to name a repo and branch. An entry names a DIRECTORY, not a
// file list, so this is how the installer learns what is in one — from the repository itself, which is the
// only source that cannot drift from it (a `files: []` array in index.json would be a second copy of the
// same truth, maintained by hand, which is what issue #57 was about).
QString treeApiUrl(const QString& indexUrl);

// The outcome of reading a Trees API response. Either a usable file list or a user-facing reason there
// isn't one — never an empty list that reads as success.
struct Listing {
    QStringList files;   // paths RELATIVE to dir
    QString     error;   // non-empty => not installable, and this is what the row shows
    bool ok() const { return error.isEmpty(); }
};

// Filter a Trees API response to the blobs under `dir/`. Refuses (with a reason) when the response is
// truncated, `dir` holds no theme.json of its own, the folder is absent, any path is unsafe, two paths
// differ only in case, there are more than kMaxFiles files, or any file exceeds kMaxFileBytes — or does not
// state a size at all, which is the same thing said less honestly. One bad file fails the WHOLE entry: a
// theme installed without its font is a broken theme, and skipping quietly would produce one.
Listing filesUnder(const QByteArray& treeJson, const QString& dir);

// The download URL for one listed file: `base`/`dir`/`rel`, with every segment percent-encoded so a theme
// shipping a font or sound with a space in its name resolves. Shared by both surfaces — the encoding is the
// fiddly part of the otherwise-trivial download loop, and it is exactly the sort of thing that gets fixed
// on one surface and not the other.
QString assetUrl(const QString& base, const QString& dir, const QString& rel);

// The caps applied to bytes that have ACTUALLY ARRIVED, which is a different question from the one
// filesUnder answers. filesUnder checks the size the TREE RESPONSE CLAIMS; the blobs are fetched afterwards,
// as separate requests against branch HEAD, so a registry that commits small files, gets listed, and then
// force-pushes large ones serves whatever it likes. Nothing downstream bounds the read — both surfaces call
// reply->readAll() — so the only place the promise can be kept is here, after each file is in hand.
//
// `soFar` is the entry's running total BEFORE this file, so the total budget is enforced across the whole
// folder rather than one file at a time. Returns false and fills *error with the user-facing reason.
//
// It lives in ThemeRegistry rather than in either caller for the reason the rest of this unit does: there
// are two download loops, they are in different files, and a cap enforced on one of them is not a cap.
bool acceptDownloadedBytes(qint64 bytes, qint64 soFar, const QString& rel, QString* error);

// Where theme FOLDERS live, given the app's writable data dir: `dataDir/themes2`. ThemeEngine (the picker),
// AssetBootstrap (first-run extraction and the XMB retirement) and the gallery dialog each spelled this out
// separately, and the failure mode of a fourth copy is silent: the installer writes into one directory and
// the picker scans another, so an install "succeeds" and the theme is nowhere.
//
// Takes the data dir rather than calling AppPaths::dataDir() so this stays a pure function of its inputs —
// which is what keeps ThemeRegistry QtCore-only and network-free, and what lets probe_themereg pin it
// without the probe data-dir isolation shim in the way. AssetBootstrap is already parameterised the same
// way and for the same reason.
//
// Empty in, empty out: an unknown data dir must not resolve to "/themes2" at the root of the filesystem.
// installFiles already refuses an empty root with a reason, so the emptiness propagates to a message.
QString themesRoot(const QString& dataDir);

// Write a downloaded theme folder into `themesRoot/folder`, replacing any existing folder of that name.
// Writes into a staging directory and renames into place, so an interrupted or refused install never leaves
// a partial folder where it would be picked up — ThemeEngine::availableThemes() offers any SUBDIRECTORY of
// themesRoot holding a theme.json, and a theme with missing sounds and fonts would be selectable. Refuses an
// empty file set, an unsafe folder name, the reserved staging name, a name ending in the parked-copy suffix,
// any unsafe relative path and any case-insensitive collision, without having touched the existing install.
// Subdirectories are PRESERVED.
//
// The suffix refusal is not tidiness. The parked copy of "Keep" lives at <staging>/Keep.replaced, and the
// staging path of a folder NAMED "Keep.replaced" is the same string — so installing an entry whose `dir` is
// "themes2/Keep.replaced" (a plain segment, and a name anyone may open a pull request for against a public
// registry) would have this function's opening tidy-up delete the one folder the parking rule exists to
// preserve, whether or not that install then went on to succeed.
//
// The one case where a refusal cannot leave the existing install where it was is a swap that half-completed:
// the old folder was moved aside and neither the new copy nor the old one could be moved back. The old
// folder is then LEFT in staging rather than deleted, and *error names its path — a theme the user can move
// back by hand is a recoverable failure; deleting it because the tidy-up is unconditional is not.
//
// That parked copy survives a RETRY, which is what makes it a guarantee rather than a one-call reprieve: a
// later call that finds it with the destination still EMPTY puts it back before staging anything, and, if it
// still cannot, refuses with the same message rather than clearing it away — the cause of a double-rename
// failure is usually persistent, so pressing Install again is the same roll. A parked copy whose destination
// is occupied is the residue of a swap that DID succeed, and only that one is removed.
bool installFiles(const QString& themesRoot, const QString& folder,
                  const QVector<QPair<QString, QByteArray>>& files, QString* error);

} // namespace ThemeRegistry
