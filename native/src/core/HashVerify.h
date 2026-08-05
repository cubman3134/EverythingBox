// ROM dump verification against user-supplied No-Intro / Redump DAT files (issue #97). Given a ROM and one or
// more Logiqx-format DAT files the user has dropped into <data>/dats/, decide whether this dump is *good*:
//
//   * Verified — a DAT entry's CRC32/MD5/SHA1 matches this ROM's PAYLOAD hash.
//   * Bad      — a DAT covers a game of this name but no hash matches (the interesting case: a corrupt or
//                over-dumped file that presents as mysterious emulation bugs users blame on us).
//   * Unknown  — no DAT covers it at all (neutral, NOT scary — the user simply has no DAT for that system).
//
// We never redistribute DATs (No-Intro's terms make that the user's download); this only *consumes* them.
//
// The correctness core is that we hash the ROM *payload*, not the raw file bytes, honouring the format quirks
// that make naive hashing wrong:
//   * iNES (.nes) files carry a 16-byte header that is NOT part of the payload — a good dump would read as Bad
//     if you hashed the whole file. payloadBytes() strips it (detected by the "NES\x1A" magic).
//   * a ROM inside a .zip/.7z is hashed from the EXTRACTED stream, not the archive bytes — the app-side verify
//     pass extracts via ArchiveRom first, then calls this on the extracted file, so this module stays free of
//     the archive/compression dependencies (and the probe links only Qt6::Core).
//   * a CHD carries its own SHA1 in its (trivially-laid-out) v5 header — chdSha1FromHeader() reads it straight
//     from the header rather than decompressing the whole disc image.
//
// Split, like Miximage/RomPatch, into PURE testable functions (parse / hash / classify — no clock, no ini, no
// disk beyond the file handed in) and thin glue (parseDatDir reads a folder; the verify cache persists a
// per-ROM stamp by path+mtime so a 4 GB ISO is hashed once, not per scan).
#pragma once
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

namespace HashVerify
{
    // The three dump outcomes. Ordered so a "worse" verdict never silently downgrades to a milder one when
    // merged, and so Unknown (the neutral default) is the zero value.
    enum class Status { Unknown, Verified, Bad };

    QString statusToken(Status s);   // "verified" / "bad" / "unknown" — the stable stamp string
    Status  statusFromToken(const QString& t);

    // A ROM's payload hashes. All lower-case hex; crc is 8 chars, md5 32, sha1 40. Any may be empty (e.g. the
    // CHD path fills only sha1).
    struct Hashes
    {
        QString crc;   // CRC32, zlib polynomial, 8 hex chars
        QString md5;   // 32 hex chars
        QString sha1;  // 40 hex chars
        bool isEmpty() const { return crc.isEmpty() && md5.isEmpty() && sha1.isEmpty(); }
    };

    // One <rom> row parsed out of a DAT: its owning game's name plus whichever of the three hashes the DAT
    // carried (lower-cased; absent ones stay empty).
    struct DatEntry
    {
        QString game;  // the <game>/<machine> name attribute
        QString crc;
        QString md5;
        QString sha1;
    };

    // A parsed (and possibly merged-from-many-files) DAT database, with hash and name indexes for O(1) lookup.
    struct DatDb
    {
        QVector<DatEntry> entries;
        QHash<QString, int> byCrc;   // lower-hex hash -> index of first entry carrying it
        QHash<QString, int> byMd5;
        QHash<QString, int> bySha1;
        QSet<QString>       names;   // normalized (see normalizeName) game names present, for the Bad test

        bool isEmpty() const { return entries.isEmpty(); }
        // True if ANY of the supplied payload hashes matches an entry (sha1 preferred, then md5, then crc).
        bool matchesHash(const Hashes& h) const;
        bool hasName(const QString& normalizedName) const { return names.contains(normalizedName); }
    };

    // ---- 1. DAT parsing (pure) -----------------------------------------------------------------------------
    // Parse one Logiqx XML DAT buffer. Accepts <game> and <machine> elements; each <rom> becomes an entry.
    // Malformed XML yields whatever parsed before the error (never throws). Deterministic.
    DatDb parseDat(const QByteArray& xml);
    // Merge another DAT buffer's entries into an existing db (rebuilding indexes as it goes).
    void  mergeDat(DatDb& into, const QByteArray& xml);
    // Read + merge every *.dat / *.xml in a folder (the app points this at <data>/dats/). Missing dir -> empty.
    DatDb parseDatDir(const QString& dir);

    // Normalize a game / file name for the name index and the Bad test: lower-cased, a trailing ROM extension
    // stripped, runs of whitespace collapsed, trimmed. Pure.
    QString normalizeName(const QString& name);

    // ---- 2. Payload hashing (pure) -------------------------------------------------------------------------
    // The bytes that actually get hashed, given a format hint (a system id like "nes" or a file extension like
    // ".nes" — either works, and it is only advisory). For an iNES file (leading "NES\x1A" magic) this returns
    // the buffer with its 16-byte header removed; for everything else it returns the buffer unchanged.
    QByteArray payloadBytes(const QByteArray& raw, const QString& formatHint = QString());
    // CRC32 + MD5 + SHA1 of EXACTLY these bytes (no header logic — feed it a payload). Pure.
    Hashes hashBytes(const QByteArray& payload);
    // Convenience: hashBytes(payloadBytes(raw, hint)).
    Hashes hashPayload(const QByteArray& raw, const QString& formatHint = QString());

    // The SHA1 a CHD stores in its own header. Parses the v5 header layout (magic "MComprHD"); returns the
    // 40-hex overall SHA1 (the field a MAME/clrmamepro CHD DAT lists), or empty for a non-CHD / truncated /
    // non-v5 buffer. `header` need only be the first ~124 bytes. Pure — no decompression.
    QString chdSha1FromHeader(const QByteArray& header);

    // ---- 3. Classification (pure) --------------------------------------------------------------------------
    // Verified if a payload hash is in the DAT; else Bad if a DAT entry's game name matches this ROM's name
    // (a DAT knows this game but the bytes are wrong); else Unknown. `romNameNoExt` is the ROM's display /
    // file base name (normalizeName is applied internally).
    Status classify(const Hashes& h, const DatDb& db, const QString& romNameNoExt);

    // ---- glue: file hashing + the per-ROM stamp cache ------------------------------------------------------
    // Hash a real ROM file's payload. Handles plain files (with the iNES header skip) and .chd (header SHA1,
    // no decompression). Does NOT open archives — the caller extracts via ArchiveRom and passes the extracted
    // file. Empty Hashes with *error set on an unreadable file. `systemHint` is the format hint.
    Hashes hashRomFile(const QString& path, const QString& systemHint, QString* error = nullptr);

    // Is the whole feature switched on? (Settings::verifyRoms — checked by the app-side verify pass; kept out
    // of the pure functions so the probe never needs Settings.)

    // The folder the user drops DAT files into: <data>/dats/ (created on first use by the app).
    QString datsDir();

    // Per-ROM verify stamp, cached by path + mtime + size so a large ISO is hashed once. Persisted in the
    // portable everythingbox.ini under "hashverify/". A cache miss returns {Unknown, valid=false}; the app-side
    // pass computes + stores. Kept here (not the display path) so the badge read is a cheap ini lookup.
    struct Stamp
    {
        Status  status = Status::Unknown;
        QString sha1;          // the payload sha1 we computed (identity anchor across renames — issue #97)
        QString datGame;       // the canonical DAT name when Verified ("" otherwise)
        bool    valid = false; // false = never verified (distinct from a real Unknown result)
    };
    Stamp cachedStamp(const QString& path);                       // cheap read for the badge (path+mtime gated)
    // Compute + persist a stamp for `path`. The bytes hashed come from `hashSourcePath` when it is non-empty
    // (the app passes the ArchiveRom-extracted temp file for a zipped ROM), while the stamp stays KEYED and
    // mtime/size-GATED on `path` itself — so the cache tracks the archive the user sees, not a throwaway temp.
    // When `hashSourcePath` is empty the file at `path` is both hashed and stamped (the plain-file case).
    Stamp verifyAndCache(const QString& path, const QString& systemHint, const DatDb& db,
                         const QString& hashSourcePath = QString());
    void  clearCache();                                           // drop all stamps (e.g. DATs changed)
}
