// DecorationPack — the pure core of the decoration (bezel) pack gallery: what a registry index's
// `decorations` section means, what a pack's zip is allowed to contain, and where its files land.
//
// A decoration pack is the THIRD install mechanism this app's registry browser serves (issue #187), next to
// the add-on file-list and the themes2 folder-tree of #91. It is one zip carrying bezel art for one or more
// emulated systems, and it unpacks into <data>/bezels/<system>/<packId>/… — one folder per system, so
// removing a pack is a folder delete and two packs can never overwrite each other's default.png.
//
// Everything here is QtCore-only and network-free, for the same reason ThemeRegistry is: BOTH surfaces (the
// classic RegistryBrowser and the themed presentDecorationRegistry) install through it, so they cannot
// disagree about which index key is authoritative, which strings may become filenames, or which systems a
// pack claims — and probe_themereg / probe_decopack can pin all of it headlessly.
//
// The one part that is NOT here is the unzip: it needs miniz, and there is already a whole-archive
// extractor with a zip-slip guard (ArchiveRom::extractAll / ArchiveSafePath::join). DecorationInstall glues
// this unit to that one. What lives here is the DECISION the unzip cannot make for itself — planInstall(),
// which turns a list of member names into "this file, for that system", including the single-top-level-
// folder strip and the refusal of anything that is not a system this app knows about.
#pragma once
#include <QByteArray>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace DecorationPack {

// A pack's zip download budget. A bezel pack is PNG art: a lavish 4K shell for a dozen systems is a few
// tens of megabytes, and the registry is public — anyone may open a pull request against one. This is the
// number the two download loops hand the network layer (they cap the reply's read buffer at it and abort
// the transfer when downloadProgress passes it), so it bounds a hostile pack while the response is arriving
// rather than after it is resident. It is deliberately larger than ThemeRegistry::kMaxTotalBytes — a theme
// is JSON plus a handful of assets, a bezel pack is nothing but full-screen images — and deliberately far
// short of "whatever the server sends", on a product that ships to a 32-bit armv7 Android TV box.
inline constexpr qint64 kMaxZipBytes = 64 * 1024 * 1024;

// The most files one pack's zip may unpack to, and the largest one member may be once decompressed. A zip
// bomb is a 40 KB download that writes terabytes; the extractor streams to disk, so nothing here bounds
// memory — these bound the DISK the user's data directory can lose to one press of Install.
inline constexpr int    kMaxMembers        = 512;
inline constexpr qint64 kMaxUncompressedBytes = 512 * 1024 * 1024;

// One entry of the index's `decorations` array.
//
// `id` is the install folder name, so it is held to the same "may this become a filename?" rule as a theme
// folder — see ThemeRegistry::isSafeRelPath, which this unit reuses rather than restating.
// `sha256` is REQUIRED and is verified against the downloaded bytes before a single file is written. A pack
// is an opaque binary from a public registry over a URL the index chose; without the digest, "install this
// zip" is "run whatever that host serves today". An entry without a well-formed digest is DROPPED by
// parseDecorations rather than offered with the check skipped.
struct Entry {
    QString     id;         // install folder name under bezels/<system>/ — a plain path segment
    QString     name;       // display text ONLY — never used as a path
    QStringList systems;    // EB system ids this pack carries bezels for; what the browse surface filters on
    QString     author;
    QString     license;
    QString     version;
    QString     zip;        // the pack zip's URL: absolute, or relative to the index URL's directory
    QString     sha256;     // 64 lowercase hex digits, REQUIRED
    QString     preview;    // optional preview image URL; advisory only
    qint64      size = 0;   // the zip's advertised size in bytes; advisory only, never trusted

    bool isValid() const;
};

// The outcome of reading the `decorations` section — the same two-answers-in-one-value shape (and the same
// reason) as ThemeRegistry::Index, per issue #174. "This registry offers no decoration packs" and "this is
// not a document I recognise" are different facts, and the second is only ever discovered by someone told
// it, so an empty list is not allowed to stand for both.
struct Index
{
    QVector<Entry> entries;
    QString        shapeError;   // empty => understood, INCLUDING understood-and-empty
    bool ok() const { return shapeError.isEmpty(); }
};

// Parse the `decorations` section of a registry index.
//
// A MISSING `decorations` key is NOT an error and yields an empty list. That is the one deliberate
// difference from ThemeRegistry::parseIndex, and it is not laxity: decorations live in the SAME index
// document as themes2, so every registry that predates this feature — and every registry that only ever
// wants to serve themes — is a document this reader understands perfectly and which has no packs in it.
// Treating that as a shape error would put a red "could not be read" card next to every theme in the
// default registry.
//
// These DO come back with a shapeError, because none of them is a registry saying "no packs":
//   * anything that is not a JSON object at the top level — unparseable bytes, or the HTML error page a
//     misconfigured host serves with a 200;
//   * a `decorations` key whose value is not an array;
//   * an array that held elements and yielded no installable entry — every one of them dropped. That is the
//     ENTRY shape drifting rather than the container's, and it is exactly as invisible as the container
//     drifting if both just produce an empty list. The message says how many were dropped and why the first
//     one was.
// A `decorations` array that is genuinely EMPTY is not an error: that registry is saying it has nothing.
Index parseDecorations(const QByteArray& json);

// 64 lowercase hex digits. Uppercase is REFUSED rather than folded: the digest is compared against a
// lowercase QCryptographicHash::toHex(), and an index that ships it uppercase should be fixed in the index
// rather than silently accommodated at every future comparison site.
bool isSha256Hex(const QString& s);

// The SHA-256 of `bytes` as lowercase hex.
QString sha256Hex(const QByteArray& bytes);

// Where decoration packs live, given the app's writable data dir: `dataDir/bezels`. This is the SAME
// directory RetroView already resolves bezel art out of (issue #106) — stated once here so the installer
// cannot write into one directory while the renderer scans another, which is the silent failure mode
// ThemeRegistry::themesRoot exists to prevent for themes.
//
// Empty in, empty out: an unknown data dir must not resolve to "/bezels" at the root of the filesystem.
QString bezelsRoot(const QString& dataDir);

// bezelsRoot/system/packId, or empty when either name is unusable as a path segment. The single place that
// spells this layout, because #106's selection, the installer and the uninstaller all have to agree on it.
QString packDir(const QString& root, const QString& system, const QString& packId);

// One file of a planned install: the member as the zip names it, and where it goes.
struct Item {
    QString member;   // the member's name inside the zip, exactly as the archive spells it
    QString system;   // the EB system id it belongs to
    QString rel;      // its path RELATIVE to bezels/<system>/<packId>/
};

// What planInstall decided. `error` non-empty means REFUSE THE WHOLE PACK — never install part of one: a
// pack that landed without its info files is a pack whose bezels place the game in the wrong rectangle.
struct Plan
{
    bool           stripTop = false;   // a single top-level wrapper folder was removed
    QString        topFolder;          // …this one; empty when nothing was stripped
    QStringList    systems;            // the system folders found, sorted and deduped
    QVector<Item>  items;              // every file that will be written
    QStringList    ignored;            // top-level names that are not a system this app knows about
    QString        error;
    bool ok() const { return error.isEmpty(); }
};

// Turn a zip's member names into an install plan, given the system ids this app knows about.
//
// TWO LAYOUTS are accepted, which is the whole reason this is a function rather than a loop at the call
// site. A pack zipped from inside its own folder has `snes/…`, `nes/…` at the root; a pack zipped from
// outside it has `MyPack/snes/…`, and every archiving tool on every platform produces the second by
// default. So: when EVERY member sits under ONE top-level folder AND that folder is not itself a known
// system id, it is stripped.
//
// The "not a known system id" clause is the load-bearing half. A single-system pack's root IS one folder —
// `snes/` — and stripping that would leave the files with no system at all and the pack refused as empty.
// The strip therefore asks what the folder MEANS, not how many there are.
//
// Everything else at the top level after the strip — a README, a `_MACOSX` directory, art for a system this
// build has never heard of — is IGNORED and named in `ignored`, not refused: a pack that carries one extra
// system is still a good pack for the systems it does carry, and refusing it would make every pack hostage
// to the newest console in the registry.
//
// A member whose name would escape the destination (`..`, an absolute path, a drive letter, a UNC path) is
// refused OUTRIGHT — the whole plan, not just that member. That check is ArchiveSafePath::join's, reached
// through the same header the real extractor uses, so there is exactly one zip-slip rule in the product.
Plan planInstall(const QStringList& members, const QStringList& knownSystems);

// ---- what is on disk -------------------------------------------------------------------------------

// One installed pack, as read back from the bezels directory.
struct Installed {
    QString     id;
    QString     name;
    QString     version;
    QString     author;
    QStringList systems;   // the systems it is ACTUALLY installed for — the directory layout, not its claim
};

// Every pack installed under `root`, sorted by id. The authority is the DIRECTORY LAYOUT: a pack is
// installed for system S if bezels/S/<id>/ exists. The pack.json written beside the art only supplies the
// display fields, so a hand-deleted system folder immediately stops being claimed.
QVector<Installed> installedPacks(const QString& root);

// The ids of the packs installed for one system, sorted. This is what BezelSelect::candidates() is handed,
// so the order in which two packs compete for the same system is stable across runs rather than being
// whatever the filesystem enumerates first.
QStringList packsForSystem(const QString& root, const QString& system);

// The JSON written into every installed pack folder, so installedPacks can name a pack rather than only
// list its id. Kept here (rather than in the installer) because the reader is here.
QByteArray packManifest(const Entry& entry);

// Delete every bezels/<system>/<packId> folder. Refuses an unusable id without touching anything, and
// returns false with a reason when the pack was not installed or a folder could not be removed. On a
// partial failure it reports which system folder is left, rather than claiming a clean uninstall.
bool removePack(const QString& root, const QString& packId, QString* error);

} // namespace DecorationPack
