// "Send library to device" (issue #127): warming another EverythingBox's art cache over the LAN, so a TV box
// does not re-scrape thousands of images the desktop already has on disk.
//
// This file is the WHOLE of the feature's thinking — the stamp, the inventory diff, the wire payload, the
// safety rules and the atomic landing — expressed as data in / data out. It pulls in QtCore only (QDir and
// QFile are QtCore; there is no QtNetwork here and no window), so probe_bundlexfer drives every decision with
// no socket and no second machine. The live halves are deliberately thin: PlayOnClient carries the bytes over
// #143's already-paired transport, RemoteServer routes GET /inventory and POST /bundle into the two hooks
// below, and MainWindowSendLibrary.cpp is the menu.
//
// THE FIVE DECISIONS, and why each is here rather than in a caller:
//
//   1. THE STAMP. One item's identity as content: a SHA-256 over its `meta.json` BYTES plus, for every other
//      transferable file, its name, size and modification time. That is issue #127's own definition, and the
//      reason it is not a hash of every art byte is arithmetic: a 2,000-game library is gigabytes of PNG, and
//      an inventory that has to read all of it is slower than the re-scrape it exists to avoid. The cost of
//      that choice is stated rather than hidden — an art file rewritten to the SAME SIZE with its mtime
//      restored is invisible to the stamp, and probe_bundlexfer pins exactly that as a known limit rather
//      than pretending otherwise.
//
//   2. MTIMES TRAVEL, and this is what makes the feature's defining property true. A landed file gets the
//      SOURCE'S modification time, so the target recomputes the SAME stamp the source sent. Without that,
//      every file would look changed the moment it arrived and the second run would ship the whole library
//      again — the exact behaviour this issue exists to remove. A filesystem that refuses to set a time
//      (or rounds it, as FAT does) degrades to re-sending that item next run: slower, never wrong.
//
//   3. THE DIFF IS THE FEATURE. planTransfer() sends an item only when the target LACKS it, or holds a
//      different stamp AND an older `updatedMs`. Equal stamps send nothing; a target item that is NEWER is
//      kept, not clobbered. Warm, never fight.
//
//   4. NOTHING BUT ART MOVES. The transferable set is an EXTENSION ALLOWLIST (meta.json + image roles + xml),
//      so a trailer, a theme song and a manual — the megabyte roles — stay where they are, and an item id
//      that is not a 40-character MetaCache hash is refused outright. There is no path in sections 1-10 that
//      can name a directory outside the cache root, and no state store (marks, favourites, resume) is opened
//      at all: this moves art, and art is the only thing it can move. (Section 11, issue #292, is gamelist
//      sidecars: a separate payload kind with its own root -- the target's ROM folder -- and its own guard,
//      and no function there is handed the cache root.)
//
//   5. LANDING IS ATOMIC PER ITEM. Files are staged in `.eb-incoming/<id>`, the live folder is set aside in
//      `.eb-retired/<id>`, the files the payload did NOT carry (that trailer, that manual) are moved across
//      so nothing local is lost, and only then is the staged folder renamed into place. An interruption at
//      any step leaves either the old item or the new one — never half of either — and sweepPartials() puts
//      the tree back on the next run, which is why a resumed transfer picks up from the diff instead of
//      restarting.
//
// THE RAW-BODY FORMAT (issue #291). v1 carries art as base64 inside JSON, and base64 is a third larger than
// the bytes, so an item past ~12 MiB of art could never fit the target's 20 MiB buffered request -- it was
// skipped on every run, and the second run never came up empty. Payload format v2 carries the SAME item as a
// binary body instead:
//
//     offset 0      8 bytes   magic "EBBUNDLE"
//     offset 8      u32 BE    format version (2)
//     offset 12     u32 BE    header length N (1 .. kMaxV2HeaderBytes)
//     offset 16     N bytes   compact JSON: {"id","stamp","updated","files":[{"name","mtime","size"},...]}
//     offset 16+N   the file bytes, concatenated in header order
//
// The decision stays here and stays pure: decodeHeaderV2() validates the WHOLE header -- every refusal v1
// has, plus the rule that the declared sizes sum to exactly the bytes that follow -- before a single file
// byte is read, and landItemV2() only starts staging once it has. The target never buffers a v2 body:
// RemoteServer spools it to a file under the cache root (openSpool) and lands it from there.
//
// v1 stays, on both ends, forever. The inventory keeps "v":1 (an old source refuses a higher one) and says
// it can take v2 in a field an old parser ignores ("bundle":[1,2]); a source sends v2 only to a target that
// says so, and v1 -- with v1's 12 MiB item cap, skipping exactly what it skipped before -- to one that
// does not.
#pragma once
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

class QFile;
class QIODevice;

namespace LibraryBundle
{
    // ---- 0. limits and versions ---------------------------------------------------------------------------

    // The payload format version. A bundle from a FUTURE format is refused with a readable message rather
    // than half-understood (#127's "no cross-version cache translation" guard).
    constexpr int kFormatVersion = 1;

    // A single file over this is not art, it is media: excluded from the scan AND from the payload, on BOTH
    // sides, so the two ends still agree on the stamp. A whole item's transferable bytes over the second cap
    // is skipped by the source and refused by the target.
    constexpr qint64 kMaxFileBytes = 8LL * 1024 * 1024;
    constexpr qint64 kMaxItemBytes = 12LL * 1024 * 1024;

    // The staging and set-aside folders, both directly under the cache root. Neither is a valid item id
    // (see safeItemId), so the inventory walk skips them without a special case.
    constexpr const char* kIncomingDir = ".eb-incoming";
    constexpr const char* kRetiredDir  = ".eb-retired";
    constexpr const char* kStampCache  = ".eb-bundle-stamps.json";

    // ---- 1. safety ----------------------------------------------------------------------------------------

    // A MetaCache item folder is sha1(key).toHex(): exactly 40 lowercase hex characters. Requiring that shape
    // is the whole traversal defence — "..", an absolute path, a drive letter and a UNC share all fail it,
    // and they fail it before any of them reaches QDir.
    bool safeItemId(const QString& id);

    // A bare file name inside an item folder: no separator, no "..", no leading dot, no Windows device name,
    // and an extension on the art/metadata allowlist. This is what keeps an executable, a trailer and a
    // manual out of a bundle.
    bool safeFileName(const QString& name);

    // ---- 2. the stamp -------------------------------------------------------------------------------------

    struct FileEntry
    {
        QString name;          // bare name inside the item folder
        qint64  size = 0;
        qint64  mtimeMs = 0;   // ms since epoch, UTC
    };

    // The item's content stamp. `files` is the transferable set (sorted or not — this sorts); `metaJson` is
    // the exact bytes of meta.json, whose CONTENT is hashed while every other file contributes name+size+mtime.
    // Deterministic across machines and across runs, which is the only property the diff needs of it.
    QString stampOf(const QList<FileEntry>& files, const QByteArray& metaJson);

    // The newest modification time in the set — the ORDER the "keep the newer one" rule needs, since two
    // stamps are hashes and hashes do not compare. 0 for an empty set.
    qint64 updatedMsOf(const QList<FileEntry>& files);

    // ---- 3. the inventory ---------------------------------------------------------------------------------

    struct Entry
    {
        QString id;
        QString stamp;
        qint64  updatedMs = 0;
    };

    QByteArray inventoryJson(const QList<Entry>& entries);
    bool parseInventory(const QByteArray& json, QList<Entry>& out, QString& error);

    // ---- 4. the diff --------------------------------------------------------------------------------------

    enum class Verdict { Send, Unchanged, TargetNewer };

    // One item's answer. `target` is null when the target does not have the item at all.
    Verdict verdictFor(const Entry& source, const Entry* target);

    struct Plan
    {
        QStringList send;          // ids to transfer, in the source's order
        QStringList unchanged;     // identical stamp on both ends — the second run's whole result
        QStringList targetNewer;   // the target's copy wins and is left alone
    };

    Plan planTransfer(const QList<Entry>& source, const QList<Entry>& target);

    // ---- 5. the payload -----------------------------------------------------------------------------------

    struct PayloadFile
    {
        QString    name;
        qint64     mtimeMs = 0;
        QByteArray data;
    };

    struct Payload
    {
        int     version = kFormatVersion;
        QString id;
        QString stamp;             // the SOURCE's stamp, carried so the target can answer with it later
        qint64  updatedMs = 0;
        QList<PayloadFile> files;
    };

    QByteArray encodePayload(const Payload& p);

    // UnsupportedKind (#292): a v2 body of a kind this decoder does not land -- a gamelist entry handed to the
    // art-cache decoder, or the other way round. Refused with a sentence, never half-understood.
    enum class Refusal { None, Malformed, FutureFormat, UnsafeId, UnsafeFileName, TooLarge, Empty, UnsupportedKind };

    // Decode + validate in one call: a payload that survives this is safe to land. `why` names the refusal and
    // `message` is the sentence the source is shown (never a path, never a credential).
    bool decodePayload(const QByteArray& bytes, Payload& out, Refusal& why, QString& message);

    // ---- 6. the disk ---------------------------------------------------------------------------------------

    QString itemDir(const QString& root, const QString& id);

    // The transferable files of one item, and its stamp. false when the folder is absent or holds nothing
    // transferable (an item with no meta.json and no art is not worth a round trip).
    bool scanItem(const QString& root, const QString& id, Entry& entry, QList<FileEntry>& files);

    // Every item under `root`, stamped. Sweeps any interrupted landing first (so a resumed run reports the
    // tree it actually has) and keeps a stamp cache beside the items so an unchanged item costs a directory
    // listing rather than a meta.json read.
    QList<Entry> inventoryFor(const QString& root);

    // Read one item into a payload ready for the wire. false + `error` when the item is absent or its
    // transferable bytes exceed kMaxItemBytes.
    bool readPayload(const QString& root, const QString& id, Payload& out, QString& error);

    // NotApplicable (#292): a gamelist entry for a system folder or a ROM this device does not have. Nothing is
    // written, and it is counted as such -- not reported as a failure.
    enum class LandResult { Landed, KeptNewer, AlreadyCurrent, Refused, Interrupted, WriteFailed, NotApplicable };

    // Test seam for the interruption case, and nothing else: fail after this many files have been staged
    // (-1 = never). It exists because "an interrupted transfer leaves the target consistent" is a promise
    // that cannot be made honestly without a way to interrupt one on purpose.
    struct LandOptions
    {
        int failAfterFiles = -1;
    };

    // Land one item under `root`. Recovers any earlier interruption of THIS id first, honours the
    // keep-the-newer rule, preserves the local files the payload did not carry, and swaps the folder in with
    // renames. Writes nothing outside `root`.
    LandResult landItem(const QString& root, const Payload& p, QString& error);
    LandResult landItem(const QString& root, const Payload& p, QString& error, const LandOptions& opts);

    // Put the tree back after an interrupted landing: a retired folder whose live folder is missing is moved
    // back, everything else staged or retired is dropped. Returns how many items it recovered or discarded.
    int sweepPartials(const QString& root);

    // ---- 7. the answers -----------------------------------------------------------------------------------

    struct Receipt
    {
        int     httpStatus = 200;
        QString result;    // "landed" | "kept" | "current" | "refused" | "failed"
        QString reason;    // "" when it landed
    };

    Receipt receiptFor(LandResult r, const QString& message);
    QByteArray receiptJson(const Receipt& r);
    bool parseReceipt(const QByteArray& json, Receipt& out);

    // ---- 8. progress --------------------------------------------------------------------------------------

    struct Progress
    {
        int    itemsTotal   = 0;   // how many the plan said to send
        int    itemsSent    = 0;
        int    keptNewer    = 0;   // the target's copy was newer
        int    unchanged    = 0;   // identical stamp; never left this machine
        int    failed       = 0;
        qint64 bytesSent    = 0;   // #292: includes the gamelist images sent
        // #292: the gamelist half of the run. `gamelists` is false when it did not run (a target that does not
        // take gamelist entries), and then the sentence says nothing about them.
        bool   gamelists          = false;
        int    gamesAdded         = 0;
        int    gamesListed        = 0;   // already in the target's gamelist; never touched
        int    gamesNotApplicable = 0;   // the target has no such system folder or no such ROM
        int    gamesFailed        = 0;
    };

    // Bytes as a sentence. Kilobytes below a megabyte: a run that moved 300 KB of PNG reporting "0.0 MB"
    // reads as a run that moved nothing, and "nothing" is exactly what the other message here means.
    QString describeSize(qint64 bytes);

    // Both ends are honest about what happened: item counts AND bytes, including the run that moved nothing —
    // and INCLUDING the items the target kept because its own were newer, which is a decision the source made
    // and must not report as an absence of one.
    QString describeProgress(const Progress& p, const QString& deviceName);

    // ---- 9. the raw-body format (issue #291) --------------------------------------------------------------

    constexpr int    kPayloadFormatV2  = 2;
    // v2's own item ceiling. kMaxFileBytes is deliberately NOT raised: it decides what the stamp covers, and
    // an older device computes its stamps with 8 MiB. An item over THIS is still skipped and reported, but
    // that is now a library with pathological art rather than ordinary box art.
    constexpr qint64 kMaxItemBytesV2   = 64LL * 1024 * 1024;
    constexpr qint64 kMaxV2HeaderBytes = 256LL * 1024;
    constexpr qint64 kV2PreambleBytes  = 16;
    // The largest well-formed v2 body. RemoteServer refuses a declared Content-Length over its stream cap
    // before accepting a byte, and that cap is asserted (at compile time, in RemoteServer.cpp) to cover this.
    constexpr qint64 kMaxV2BodyBytes   = kV2PreambleBytes + kMaxV2HeaderBytes + kMaxItemBytesV2;
    constexpr const char* kBundleV2ContentType = "application/x-eb-bundle";

    // What a target's inventory says it accepts. An inventory with no "bundle" field is an older device's,
    // and an older device takes v1 only. `formats` is never empty after a successful parse.
    bool parseInventory(const QByteArray& json, QList<Entry>& out, QList<int>& formats, QString& error);

    // The source's choice: v2 when the target advertises it, v1 otherwise.
    int chooseBundleFormat(const QList<int>& advertised);

    // The item ceiling that applies to a format (v1: kMaxItemBytes; v2: kMaxItemBytesV2).
    qint64 maxItemBytesFor(int format);

    // readPayload with an explicit item ceiling -- the v2 source reads under kMaxItemBytesV2.
    bool readPayload(const QString& root, const QString& id, Payload& out, QString& error, qint64 maxItemBytes);

    // What an item puts on the target's disk: the sum of its file sizes. Progress::bytesSent counts THIS, for
    // both formats, rather than the wire size, which for v1 is a third larger than what landed.
    qint64 fileBytesOf(const Payload& p);

    QByteArray encodePayloadV2(const Payload& p);

    struct BundleHeader
    {
        int     version = kPayloadFormatV2;
        QString id;
        QString stamp;
        qint64  updatedMs = 0;
        QList<FileEntry> files;    // name, size and mtime of each file, in body order
        qint64  fileBytes = 0;     // the sum of the sizes -- exactly the bytes that follow the header
    };

    // Read and validate a v2 preamble + header from `in`, leaving it positioned at the first file byte. Every
    // refusal is decided HERE, before any file byte is read: bad magic, a missing or future version, an
    // unsafe id or file name, a file over kMaxFileBytes, an empty item, an item over kMaxItemBytesV2, and
    // declared sizes that do not sum to exactly the bytes remaining (a truncated or over-long body is
    // Malformed). `in` must be random-access -- that last check needs its size -- which is why the target
    // spools a v2 body to a file rather than reading it off the socket.
    bool decodeHeaderV2(QIODevice& in, BundleHeader& out, Refusal& why, QString& message);

    // The whole v2 payload into memory (the probe's round trip; the target lands with landItemV2 instead).
    bool decodePayloadV2(QIODevice& in, Payload& out, Refusal& why, QString& message);

    // Validate the header, then land the item by copying each file straight from `in` into the staging
    // folder -- the same recovery, keep-the-newer rule and atomic swap as landItem. A refused header returns
    // Refused having read no file byte and written nothing.
    LandResult landItemV2(const QString& root, QIODevice& in, QString& error);
    LandResult landItemV2(const QString& root, QIODevice& in, QString& error, const LandOptions& opts);

    // ---- 10. the spool (issue #291) ----------------------------------------------------------------------

    // Create and open (write-only) a fresh spool file for one incoming v2 body, INSIDE `<root>/.eb-incoming`
    // and nowhere else, and register it as in flight. Returns its path, or an empty string when the cache
    // cannot be written. The name is `bundle-<uuid>.spool`: not an item id (so the inventory walk ignores
    // it), not an image extension and not `thumb.*` (so MetaCache's image-cap sweep neither counts nor
    // evicts it).
    QString openSpool(const QString& root, QFile& file);

    // Close, delete and unregister a spool. Called on every outcome -- landed, refused, a disconnect, a
    // timeout. sweepPartials() removes any spool that is NOT registered, which after a crash is all of them.
    void discardSpool(QFile& file);

    // How many spools this process has in flight (a probe's view; nothing decides on it).
    int spoolsInFlight();

    // ---- 11. gamelist sidecars (issue #292) --------------------------------------------------------------
    //
    // An ES/RetroBat `gamelist.xml` lives beside the ROMs, not in the cache, so it is its OWN payload kind with
    // its own destination rule and its own guard. Nothing in this section takes the cache root, and nothing
    // above takes the ROM root.
    //
    //   * THE UNIT is one game: its gamelist entry plus its images, keyed by (system, ROM file name). On the
    //     source, `system` is the folder directly under romsFolder() holding the gamelist.xml; a gamelist
    //     anywhere else is not sent.
    //   * THE TARGET DECIDES WHERE IT LANDS: `<its romsRoot>/<system>/`, only when that folder already exists
    //     (it is never created) and the ROM named by the entry is a file in it. Otherwise NotApplicable.
    //     A system or ROM name that is not one safe path segment is refused before anything is written, and a
    //     path the source names is never used as a path.
    //   * WARM, NEVER FIGHT. A game the target's gamelist already lists (by GamelistStore's own rule: file
    //     name, base name, or clean title) is never touched, and the plan does not send it.
    //   * STRUCTURED FIELDS, NEVER RAW XML. The target writes the entry itself, escaped, into the existing
    //     file's bytes just before `</gameList>` (or a new file), through a temp file and a rename.
    //   * IMAGES ONLY, NAMED BY THE TARGET: `<rom base name>-thumb.<ext>` and so on, GamelistWriter's
    //     convention, in `<system>/images/`. An existing file of that name is not overwritten; that image is
    //     dropped from the entry. Videos never travel.
    //
    // WIRE. The target advertises "sidecars":[1] in its inventory (a field an older parser ignores). The
    // per-system lists travel in their own token-gated GET /gamelists, not in /inventory: a library with
    // thousands of ROMs is hundreds of kilobytes of names and a walk of the ROM tree, and the art inventory
    // (answered on a 4 s budget) should not pay for either. One game rides the v2 container on POST /bundle,
    // through the same token-first, length-first spooled path, with a header of
    //     {"kind":"gamelist","system","rom","game":{name,desc,...},"files":[{"role","ext","size"},...]}
    // followed by the image bytes in header order.

    constexpr int         kSidecarFormat       = 1;
    constexpr const char* kSidecarKindGamelist = "gamelist";
    constexpr const char* kGamelistFileName    = "gamelist.xml";
    constexpr int         kMaxSidecarImages    = 4;
    constexpr qint64      kMaxSidecarFileBytes = kMaxSidecarImages * kMaxFileBytes;

    // One path segment a device will create or look up under its ROM root: not empty, no separator, no drive
    // or stream colon, no "..", no leading dot, no trailing dot or space, no control or Windows-invalid
    // character, and not a Windows device name. Spaces, brackets and non-ASCII letters -- real ROM names --
    // are fine.
    bool safePathSegment(const QString& segment);

    // The image extensions a sidecar may carry (lower case, no dot): png, jpg, jpeg, webp, gif, bmp.
    bool sidecarImageExtension(const QString& ext);

    // The ES media roles that travel, in the order they are sent: thumbnail, image, marquee, fanart.
    QStringList sidecarImageRoles();

    // The file name the TARGET gives one image: the ROM's base name, the role's suffix (-thumb, -image,
    // -marquee, -fanart) and the extension. Empty for any other role or when the result is not safe.
    QString sidecarImageName(const QString& romName, const QString& role, const QString& ext);

    struct GamelistFields
    {
        QString name, desc, releasedate, developer, publisher, genre, players, rating;
    };

    // One <game> as it stands in a gamelist file.
    struct GamelistGame
    {
        QString path;                   // <path> exactly as stored
        QString rom;                    // the ROM file name when <path> is one segment ("./x.sfc"); else empty
        GamelistFields fields;
        QList<QPair<QString, QString>> media;   // (role, relative path as stored), video included
    };

    QList<GamelistGame> parseGamelist(const QByteArray& xml);

    // Whether `games` already lists `romName`, by the rule GamelistStore matches a ROM with.
    bool gamelistLists(const QList<GamelistGame>& games, const QString& romName);

    // SOURCE: every game in `<romsRoot>/<system>/gamelist.xml`, for each system folder directly under the root.
    struct SidecarGame
    {
        QString system;
        QString rom;
        GamelistFields fields;
        QList<QPair<QString, QString>> images;  // (role, relative path as stored), image roles only
    };
    QList<SidecarGame> sidecarGamesFor(const QString& romsRoot);

    // TARGET: per system folder, the ROM files present and which of them its gamelist already lists.
    struct SidecarSystem
    {
        QString     name;
        QStringList roms;
        QStringList listed;
    };
    QList<SidecarSystem> sidecarInventoryFor(const QString& romsRoot);
    QByteArray sidecarInventoryJson(const QList<SidecarSystem>& systems);
    bool parseSidecarInventory(const QByteArray& json, QList<SidecarSystem>& out, QString& error);

    // The capability. inventoryJson(entries) is inventoryJson(entries, false): a device only says it takes
    // gamelist entries when it has somewhere to land them.
    QByteArray inventoryJson(const QList<Entry>& entries, bool sidecars);
    bool advertisesSidecars(const QByteArray& inventoryJson);

    // The diff: send a game only when the target has the ROM and its gamelist does not list it.
    struct SidecarPlan
    {
        QList<SidecarGame> send;
        int alreadyListed = 0;
        int notApplicable = 0;
    };
    SidecarPlan planSidecars(const QList<SidecarGame>& source, const QList<SidecarSystem>& target);

    struct SidecarImage
    {
        QString    role;
        QString    ext;
        QByteArray data;
    };
    struct SidecarPayload
    {
        QString system;
        QString rom;
        GamelistFields fields;
        QList<SidecarImage> images;
    };

    // SOURCE: read one game's images. An image outside the game's system folder, missing, over kMaxFileBytes
    // or not on the image allowlist is left out. false only for a game whose names are not safe.
    bool readSidecarPayload(const QString& romsRoot, const SidecarGame& game, SidecarPayload& out, QString& error);
    qint64 fileBytesOf(const SidecarPayload& p);
    QByteArray encodeSidecarV2(const SidecarPayload& p);

    // Which decoder a v2 body belongs to, read from its header alone; the device is put back at 0.
    enum class BodyKind { Art, Gamelist, Unknown };
    BodyKind bodyKindV2(QIODevice& in);

    struct SidecarFile
    {
        QString role;
        QString ext;
        qint64  size = 0;
    };
    struct SidecarHeader
    {
        QString system;
        QString rom;
        GamelistFields fields;
        QList<SidecarFile> files;
        qint64 fileBytes = 0;
    };

    // Every refusal before a file byte is read: not a gamelist kind, an unsafe system or ROM name, a role that
    // is not an image role (a video), an extension off the allowlist, a file over kMaxFileBytes, a duplicate
    // role, and declared sizes that do not account for exactly the bytes remaining.
    bool decodeSidecarHeaderV2(QIODevice& in, SidecarHeader& out, Refusal& why, QString& message);
    bool decodeSidecarV2(QIODevice& in, SidecarPayload& out, Refusal& why, QString& message);

    // Text for an XML element: characters XML 1.0 cannot hold are dropped, and & < > are escaped.
    QString gamelistXmlText(const QString& value);
    // One <game> block, GamelistWriter's layout. `media` is (role, "./images/<name>").
    QByteArray gamelistEntryXml(const QString& romName, const GamelistFields& fields,
                                const QList<QPair<QString, QString>>& media);
    // The existing file's bytes with `block` inserted before the last </gameList>, or a new document when
    // `existing` is empty. false when a non-empty file has no </gameList> to insert before.
    bool insertGamelistEntry(const QByteArray& existing, const QByteArray& block, QByteArray& out);

    // TARGET: land one game under `romsRoot`. LandOptions::failAfterFiles counts images written; reaching it
    // at the gamelist rewrite interrupts that write (the old file stays) and removes this landing's images.
    LandResult landSidecarV2(const QString& romsRoot, QIODevice& in, QString& error);
    LandResult landSidecarV2(const QString& romsRoot, QIODevice& in, QString& error, const LandOptions& opts);
}
