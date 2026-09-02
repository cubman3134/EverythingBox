// FIXTURE BUILDERS for the reading-library probes (issue #144): real RAR 4 comic archives and real
// Palm/MOBI containers, laid out one field at a time from each format's published offsets.
//
// WHY A SHARED HEADER RATHER THAN A COPY PER PROBE. Three probes want the same fixtures at three levels —
// probe_cbr and probe_ebookformats assert the CONTAINER readers, probe_books asserts what the library SCAN
// makes of the same files — and a second implementation of a RAR writer would be a second set of bugs in the
// thing every one of those assertions rests on.
//
// NOTHING HERE INCLUDES ANYTHING FROM src/. That is the whole point: these are independent byte placers, not
// the readers' own output fed back to them. Every offset below is a literal from the format's documentation
// (RAR's block layout; the Palm database header at 78, the PalmDOC header's encryption field at record-0
// offset 12, the MOBI header's file version at 36 and full name at 84/88), and a reader that looks anywhere
// else will not find what these wrote.
#pragma once
#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>
#include <cstring>

namespace BookFixtures
{
    // ---- CRC-32 (IEEE), built here so nothing is shared with unarr's own table ---------------------------
    inline quint32 crc32Of(const QByteArray& data)
    {
        static quint32 table[256];
        static bool ready = false;
        if (!ready)
        {
            for (quint32 i = 0; i < 256; ++i)
            {
                quint32 c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            ready = true;
        }
        quint32 c = 0xFFFFFFFFu;
        for (uchar b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    // ---- Little-endian writers (RAR) ---------------------------------------------------------------------
    inline void put16(QByteArray& b, quint16 v) { b.append(char(v & 0xFF)); b.append(char((v >> 8) & 0xFF)); }
    inline void put32(QByteArray& b, quint32 v)
    {
        b.append(char(v & 0xFF)); b.append(char((v >> 8) & 0xFF));
        b.append(char((v >> 16) & 0xFF)); b.append(char((v >> 24) & 0xFF));
    }

    // ---- Big-endian writers (Palm/MOBI) ------------------------------------------------------------------
    inline void be16At(QByteArray& b, int at, quint16 v)
    {
        b[at] = char((v >> 8) & 0xFF); b[at + 1] = char(v & 0xFF);
    }
    inline void be32At(QByteArray& b, int at, quint32 v)
    {
        b[at] = char((v >> 24) & 0xFF); b[at + 1] = char((v >> 16) & 0xFF);
        b[at + 2] = char((v >> 8) & 0xFF); b[at + 3] = char(v & 0xFF);
    }
    inline QByteArray be32(quint32 v) { QByteArray b(4, '\0'); be32At(b, 0, v); return b; }

    // =====================================================================================================
    // RAR 4
    // =====================================================================================================
    struct RarEntry
    {
        QString name;
        QByteArray data;
        bool directory = false;
        // Write a WRONG data CRC-32 into the header while writing the real bytes. The block chain stays
        // perfect (so a header walk still finds the entry) and every extraction of it fails its checksum —
        // which is the only way to tell a listing that decompresses from one that does not.
        bool badCrc = false;
    };

    // A RAR block's header CRC is the LOW 16 BITS of a CRC-32 taken over the block from its TYPE byte
    // onward — i.e. skipping the two bytes the checksum itself occupies.
    inline QByteArray sealRarBlock(const QByteArray& body)   // body starts at the type byte
    {
        QByteArray out;
        put16(out, quint16(crc32Of(body) & 0xFFFF));
        out += body;
        return out;
    }

    // Signature, a 13-byte main block, then one STORED file block per entry. Method 0x30 (store) is what a
    // comic packer uses for already-compressed page images; RAR's compressed methods need a RAR compressor,
    // which this repo does not have (probe_cbr's header says so at length).
    inline QByteArray buildRar4(const QVector<RarEntry>& entries)
    {
        QByteArray out("Rar!\x1A\x07\x00", 7);
        {
            QByteArray body;
            body.append(char(0x73));            // TYPE_MAIN_HEADER
            put16(body, 0x0000);                // flags: not solid, no password, no comment
            put16(body, 13);                    // head size: 2 crc + 1 type + 2 flags + 2 size + 6 reserved
            put16(body, 0);                     // reserved1
            put32(body, 0);                     // reserved2
            out += sealRarBlock(body);
        }
        for (const RarEntry& e : entries)
        {
            const QByteArray name = e.name.toUtf8();
            QByteArray body;
            body.append(char(0x74));                                       // TYPE_FILE_ENTRY
            put16(body, quint16(0x8000 | (e.directory ? 0x00E0 : 0)));     // LHD_LONG_BLOCK (+ LHD_DIRECTORY)
            put16(body, quint16(32 + name.size()));                        // head size (7 + 4 + 21 + namelen)
            put32(body, quint32(e.data.size()));                           // packed size == unpacked (stored)
            put32(body, quint32(e.data.size()));                           // unpacked size
            body.append(char(2));                                          // host OS: Win32
            put32(body, crc32Of(e.data) ^ (e.badCrc ? 0xFFFFFFFFu : 0u));  // the DATA's own CRC-32
            put32(body, 0x4E21A800u);                                      // DOS date/time
            body.append(char(20));                                         // unpack version 2.0
            body.append(char(0x30));                                       // METHOD_STORE
            put16(body, quint16(name.size()));
            put32(body, e.directory ? 0x10u : 0x20u);                      // DOS attributes
            body += name;
            out += sealRarBlock(body);
            out += e.data;
        }
        return out;
    }

    // =====================================================================================================
    // Palm / MOBI / AZW3
    // =====================================================================================================
    struct MobiSpec
    {
        quint16 compression = 1;      // 1 = none, 2 = PalmDoc
        quint16 encryption  = 0;      // non-zero == DRM
        quint16 textRecords = 1;
        quint32 encoding    = 65001;  // UTF-8
        quint32 fileVersion = 6;      // 8 == KF8
        QString fullName;             // written after the EXTH block, pointed at by offsets 84/88
        QString author;               // EXTH 100
        int  kf8Boundary   = -1;      // EXTH 121
        int  coverOffset   = -1;      // EXTH 201
        quint32 firstImage = 0;       // MOBI header offset 108
    };

    inline QByteArray buildMobiHeaderRecord(const MobiSpec& s)
    {
        const int mobiHdrLen = 248;                 // >= 0xE4, so the extra-data-flags field is inside it
        const int exthAt = 16 + mobiHdrLen;

        QVector<QPair<int, QByteArray>> exth;       // (type, body)
        if (!s.author.isEmpty()) exth.append({ 100, s.author.toUtf8() });
        if (s.kf8Boundary >= 0)  exth.append({ 121, be32(quint32(s.kf8Boundary)) });
        if (s.coverOffset >= 0)  exth.append({ 201, be32(quint32(s.coverOffset)) });

        QByteArray exthBlock("EXTH");
        int exthLen = 12;
        for (const auto& e : exth) exthLen += 8 + int(e.second.size());
        exthBlock += be32(quint32(exthLen));
        exthBlock += be32(quint32(exth.size()));
        for (const auto& e : exth)
        {
            exthBlock += be32(quint32(e.first));
            exthBlock += be32(quint32(8 + e.second.size()));
            exthBlock += e.second;
        }

        const QByteArray name = s.fullName.toUtf8();
        const int nameAt = exthAt + exthBlock.size();

        QByteArray rec(nameAt + name.size() + 2, '\0');
        // PalmDOC header
        be16At(rec, 0, s.compression);
        be32At(rec, 4, 0);                          // text length (unused by the reader)
        be16At(rec, 8, s.textRecords);
        be16At(rec, 10, 4096);
        be16At(rec, 12, s.encryption);
        // MOBI header
        std::memcpy(rec.data() + 16, "MOBI", 4);
        be32At(rec, 20, quint32(mobiHdrLen));
        be32At(rec, 24, 2);                         // Mobipocket book
        be32At(rec, 28, s.encoding);
        be32At(rec, 32, 0x1234);                    // unique id
        be32At(rec, 36, s.fileVersion);
        be32At(rec, 84, quint32(nameAt));           // FULL NAME OFFSET — the field this reader used to miss
        be32At(rec, 88, quint32(name.size()));
        be32At(rec, 108, s.firstImage);
        be32At(rec, 128, 0x40);                     // EXTH flags: an EXTH block follows the MOBI header
        be32At(rec, 240, 0);                        // extra record data flags: none
        std::memcpy(rec.data() + exthAt, exthBlock.constData(), size_t(exthBlock.size()));
        std::memcpy(rec.data() + nameAt, name.constData(), size_t(name.size()));
        return rec;
    }

    // A whole PalmDB: a 78-byte header, an 8-byte entry per record, then the records back to back.
    inline QByteArray buildPalmDb(const QVector<QByteArray>& records)
    {
        const int listEnd = 78 + records.size() * 8 + 2;   // + the two pad bytes Palm files carry
        QByteArray out(listEnd, '\0');
        std::memcpy(out.data(), "fixture", 7);
        std::memcpy(out.data() + 60, "BOOKMOBI", 8);
        be16At(out, 76, quint16(records.size()));

        int at = listEnd;
        for (int i = 0; i < records.size(); ++i)
        {
            be32At(out, 78 + i * 8, quint32(at));
            at += records.at(i).size();
        }
        for (const QByteArray& r : records) out += r;
        return out;
    }

    // A PalmDoc stream, encoded BY HAND: literals for 0x09..0x7F, everything else through the "copy the next
    // N bytes" escape. Decoding it is the reader's job; producing it is not.
    inline QByteArray palmDocLiterals(const QByteArray& text)
    {
        QByteArray out;
        for (uchar c : text)
        {
            if (c >= 0x09 && c <= 0x7F) out.append(char(c));
            else { out.append(char(1)); out.append(char(c)); }
        }
        return out;
    }

    // A 1x1 transparent GIF — real bytes with a recognisable head, so a cover that comes back is a picture
    // and not a blob.
    inline QByteArray tinyGif()
    {
        return QByteArray::fromBase64("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7");
    }
}
