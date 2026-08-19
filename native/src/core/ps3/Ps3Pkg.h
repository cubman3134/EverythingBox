#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <optional>

// Retail PS3 .pkg entry-table access — the airtight "what must this install produce" list. A pkg's
// header names an item count and a data area; the data area begins with item_count 32-byte entries
// (name offset/size, data offset/size, type flags), everything AES-128-CTR encrypted with the fixed
// retail GPKG key. Hardware ground truth 2026-08-19 (LBP BCUS98148): an --installpkg run left
// PARAM.SFO claiming the target APP_VER=01.30 over a 0-byte USRDIR/patch.sdat and 6 missing files —
// PARAM.SFO extracts early, so only the table itself can say what a COMPLETE install looks like.
namespace Ps3Pkg {

struct Entry {
    QString path;              // relative to the game dir, e.g. "USRDIR/patch.sdat"
    qint64  size = 0;          // the table's size for this entry — what RPCS3 writes to disk for
                               // every type EXCEPT SDAT (0x09), whose on-disk file is the DecryptEDAT
                               // payload and smaller than this container size (see verifyInstalled)
    quint32 type = 0;          // the raw 32-bit type word; the low byte selects unpkg.cpp's switch
                               // case, and verifyInstalled needs it to tell "RPCS3 writes this" from
                               // "RPCS3's switch DEFAULT skips it" (see kWrittenTypes there).
    bool    isDir = false;     // type & 0xFF == 0x04/0x12 (unpkg.cpp's folder cases)
    bool    overwrite = false; // PKG_FILE_ENTRY_OVERWRITE (0x80000000): when CLEAR, RPCS3 keeps an
                               // existing file untouched ("Didn't overwrite"), so its on-disk size
                               // may legitimately differ from this entry's.
};

// AES-128-CTR keystream transform with the retail PS3 GPKG key (encrypt == decrypt). `riv` is the
// 16-byte counter base from the pkg header at 0x70; `blockOffset` positions `data` within the pkg's
// data area in 16-byte blocks (keystream block N is AES(key, riv + blockOffset + N), the addition
// 128-bit big-endian). Exposed so the probe can build encrypted fixtures with the very transform
// the parser undoes — and pin the transform itself against independent known-answer vectors, since
// a fixture round-trip alone would let a broken AES cancel itself out. Empty when riv is not 16
// bytes or blockOffset is negative — both mean a corrupt header, and a wrapped counter would hand
// back plausible garbage instead of a rejection.
QByteArray gpkgCrypt(const QByteArray& data, const QByteArray& riv, qint64 blockOffset = 0);

// Parse pkgPath's entry table. nullopt when the file is not a PS3 pkg or the decrypted table fails
// sanity (counts/offsets out of range, names not clean relative paths) — a table this key
// demonstrably did not decrypt must not drive verification, so the caller falls back to the
// 0-byte-file heuristic instead.
std::optional<QVector<Entry>> entries(const QString& pkgPath);

// After an --installpkg run: does gameDir hold everything the table names? Directories must exist;
// files must exist at exactly the expected size — except a non-overwrite entry may keep a
// pre-existing file of a different size (see Entry::overwrite), accepted only when non-empty, and
// an entry of a type RPCS3's extractor does not write at all, which is skipped entirely.
// SDAT entries (low type 0x09) are checked for presence and non-emptiness only, NOT exact size
// (a table-recorded size of 0 still requires exactly 0 on disk):
// RPCS3 writes them through DecryptEDAT, so the file on disk is the decrypted payload while the
// table records the container (A0130.pkg: 910064 in the table, 908000 installed). SDAT is the only
// buffered/decrypted type; every other type is still size-exact.
// A 0-byte file where the table expects bytes is always a failed install (the 2026-08-19 poison).
// Sizes are path-based (fresh QFileInfo): every caller runs after the installer's handles closed
// (self-exit, post-kill, the pre-spawn already-installed skip) or inside the quiet window where a
// stale directory-entry size can only FAIL the check and keep us waiting — the safe direction.
bool verifyInstalled(const QString& gameDir, const QVector<Entry>& entries);

} // namespace Ps3Pkg
