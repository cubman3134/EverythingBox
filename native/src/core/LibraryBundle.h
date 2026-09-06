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
//      that is not a 40-character MetaCache hash is refused outright. There is no path in this file that can
//      name a directory outside the cache root, and no state store (marks, favourites, resume) is opened at
//      all: this moves art, and art is the only thing it can move.
//
//   5. LANDING IS ATOMIC PER ITEM. Files are staged in `.eb-incoming/<id>`, the live folder is set aside in
//      `.eb-retired/<id>`, the files the payload did NOT carry (that trailer, that manual) are moved across
//      so nothing local is lost, and only then is the staged folder renamed into place. An interruption at
//      any step leaves either the old item or the new one — never half of either — and sweepPartials() puts
//      the tree back on the next run, which is why a resumed transfer picks up from the diff instead of
//      restarting.
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

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

    enum class Refusal { None, Malformed, FutureFormat, UnsafeId, UnsafeFileName, TooLarge, Empty };

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

    enum class LandResult { Landed, KeptNewer, AlreadyCurrent, Refused, Interrupted, WriteFailed };

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
        qint64 bytesSent    = 0;
    };

    // Bytes as a sentence. Kilobytes below a megabyte: a run that moved 300 KB of PNG reporting "0.0 MB"
    // reads as a run that moved nothing, and "nothing" is exactly what the other message here means.
    QString describeSize(qint64 bytes);

    // Both ends are honest about what happened: item counts AND bytes, including the run that moved nothing —
    // and INCLUDING the items the target kept because its own were newer, which is a decision the source made
    // and must not report as an absence of one.
    QString describeProgress(const Progress& p, const QString& deviceName);
}
