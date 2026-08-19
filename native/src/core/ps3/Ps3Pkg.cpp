#include "core/ps3/Ps3Pkg.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <cstring>

namespace {

// Self-contained AES-128 block ENCRYPTION (all CTR ever needs — decryption is the same XOR). FIPS-197
// verbatim; pinned against independent known-answer vectors in probe_ps3update's testPkgCrypt, so a
// transcription slip here goes red rather than silently rejecting every genuine pkg into the
// fallback path. Qt has no AES and the repo takes no crypto dependency for one fixed-key CTR.
const quint8 kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

quint8 xtime(quint8 x) { return quint8((x << 1) ^ ((x >> 7) * 0x1b)); }

struct Aes128 {
    quint8 rk[176]; // 11 round keys
    explicit Aes128(const quint8 key[16])
    {
        std::memcpy(rk, key, 16);
        quint8 rcon = 1;
        for (int i = 16; i < 176; i += 4)
        {
            quint8 t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
            if (i % 16 == 0)
            {
                const quint8 tmp = t[0];
                t[0] = quint8(kSbox[t[1]] ^ rcon); t[1] = kSbox[t[2]];
                t[2] = kSbox[t[3]];                t[3] = kSbox[tmp];
                rcon = xtime(rcon);
            }
            for (int j = 0; j < 4; ++j) rk[i + j] = quint8(rk[i - 16 + j] ^ t[j]);
        }
    }
    void encryptBlock(const quint8 in[16], quint8 out[16]) const
    {
        quint8 s[16];
        for (int i = 0; i < 16; ++i) s[i] = quint8(in[i] ^ rk[i]);
        for (int round = 1; round <= 10; ++round)
        {
            for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]]; // SubBytes
            // ShiftRows (state is column-major: byte r + 4c)
            quint8 t = s[1];  s[1]  = s[5];  s[5]  = s[9];  s[9]  = s[13]; s[13] = t;
            t = s[2];  s[2]  = s[10]; s[10] = t;  t = s[6]; s[6] = s[14]; s[14] = t;
            t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
            if (round != 10)
                for (int c = 0; c < 16; c += 4) // MixColumns
                {
                    const quint8 a0 = s[c], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3];
                    const quint8 x = quint8(a0 ^ a1 ^ a2 ^ a3);
                    s[c]     = quint8(a0 ^ x ^ xtime(quint8(a0 ^ a1)));
                    s[c + 1] = quint8(a1 ^ x ^ xtime(quint8(a1 ^ a2)));
                    s[c + 2] = quint8(a2 ^ x ^ xtime(quint8(a2 ^ a3)));
                    s[c + 3] = quint8(a3 ^ x ^ xtime(quint8(a3 ^ a0)));
                }
            for (int i = 0; i < 16; ++i) s[i] = quint8(s[i] ^ rk[round * 16 + i]);
        }
        std::memcpy(out, s, 16);
    }
};

// The retail PS3 GPKG data key. Validated live 2026-08-19: decrypts the entry tables of all 13
// sha1-verified LBP update pkgs into clean UTF-8 paths (rpcs3-bisect/pkgs).
const quint8 kGpkgKey[16] = { 0x2E,0x7B,0x71,0xD7,0xC9,0xC9,0xA1,0x4E,
                              0xA3,0x22,0x1F,0x18,0x88,0x28,0xB8,0xF8 };

quint16 be16(const QByteArray& b, int off) { return qFromBigEndian<quint16>(b.constData() + off); }
quint32 be32(const QByteArray& b, int off) { return qFromBigEndian<quint32>(b.constData() + off); }
quint64 be64(const QByteArray& b, int off) { return qFromBigEndian<quint64>(b.constData() + off); }

// Decrypt [off, off+len) of the data area: read the covering 16-byte blocks, transform with the
// counter positioned at the FIRST covered block, slice out the requested window.
QByteArray decryptRegion(QFile& f, quint64 dataOffset, const QByteArray& riv, quint64 off, quint64 len)
{
    const quint64 blockFirst = off / 16;
    const quint64 padded = (off + len + 15) / 16 * 16 - blockFirst * 16;
    if (!f.seek(qint64(dataOffset + blockFirst * 16))) return {};
    const QByteArray ct = f.read(qint64(padded));
    if (quint64(ct.size()) < padded) return {};
    const QByteArray pt = Ps3Pkg::gpkgCrypt(ct, riv, qint64(blockFirst));
    return pt.mid(int(off - blockFirst * 16), int(len));
}

} // namespace

namespace Ps3Pkg {

QByteArray gpkgCrypt(const QByteArray& data, const QByteArray& riv, qint64 blockOffset)
{
    if (riv.size() != 16) return {};
    // A negative offset can only come from a corrupt header; wrapping the counter would emit
    // plausible garbage, so reject it the same way a wrong-size riv is rejected.
    if (blockOffset < 0) return {};
    const Aes128 aes(kGpkgKey);
    quint8 ctr[16];
    std::memcpy(ctr, riv.constData(), 16);
    // Position the 128-bit big-endian counter at riv + blockOffset (with carry).
    quint64 carry = quint64(blockOffset);
    for (int i = 15; i >= 0 && carry; --i)
    {
        const quint64 sum = quint64(ctr[i]) + (carry & 0xFF);
        ctr[i] = quint8(sum);
        carry = (carry >> 8) + (sum >> 8);
    }
    QByteArray out(data.size(), Qt::Uninitialized);
    quint8 pad[16];
    for (qint64 base = 0; base < data.size(); base += 16)
    {
        aes.encryptBlock(ctr, pad);
        const qint64 n = qMin<qint64>(16, data.size() - base);
        for (qint64 i = 0; i < n; ++i)
            out[int(base + i)] = char(quint8(data[int(base + i)]) ^ pad[i]);
        for (int i = 15; i >= 0; --i) if (++ctr[i]) break; // big-endian increment
    }
    return out;
}

std::optional<QVector<Entry>> entries(const QString& pkgPath)
{
    QFile f(pkgPath);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray hdr = f.read(0x80);
    if (hdr.size() < 0x80) return std::nullopt;
    if (be32(hdr, 0x00) != 0x7F504B47u) return std::nullopt; // "\x7FPKG"
    if (be16(hdr, 0x06) != 0x0001) return std::nullopt;      // PS3 — the GPKG key is only theirs
    const quint32 itemCount  = be32(hdr, 0x14);
    const quint64 dataOffset = be64(hdr, 0x20);
    const quint64 dataSize   = be64(hdr, 0x28);
    const QByteArray riv = hdr.mid(0x70, 16);
    // A real update pkg names tens of entries; 100k is far past any genuine table and caps the
    // work a corrupt count can demand.
    if (itemCount == 0 || itemCount > 100000) return std::nullopt;
    // Both fields are attacker-controlled quint64s, so the containment test is written as a
    // subtraction against f.size() — `dataOffset + dataSize` can wrap and pass a torn pkg.
    if (dataOffset < 0x80 || dataOffset > quint64(f.size())
        || dataSize > quint64(f.size()) - dataOffset) return std::nullopt;
    if (quint64(itemCount) * 32 > dataSize) return std::nullopt;

    const QByteArray table = decryptRegion(f, dataOffset, riv, 0, quint64(itemCount) * 32);
    if (quint64(table.size()) != quint64(itemCount) * 32) return std::nullopt;

    QVector<Entry> out;
    out.reserve(int(itemCount));
    for (quint32 i = 0; i < itemCount; ++i)
    {
        const int o = int(i) * 32;
        const quint32 nameOff  = be32(table, o);
        const quint32 nameSize = be32(table, o + 4);
        const quint64 fileSize = be64(table, o + 16);
        const quint32 type     = be32(table, o + 24);
        if (nameSize == 0 || nameSize > 4096) return std::nullopt;
        if (quint64(nameOff) + nameSize > dataSize) return std::nullopt;
        const QByteArray nameBytes = decryptRegion(f, dataOffset, riv, nameOff, nameSize);
        if (quint32(nameBytes.size()) != nameSize) return std::nullopt;

        Entry e;
        // Trailing NULs are padding; anything else must be a clean RELATIVE path. Garbage here means
        // the key did not decrypt this table (debug pkg, foreign platform, corruption) — poison the
        // whole parse rather than verify against noise. '..'/'\\'/leading-'/' would also let a
        // hostile table walk the verifier outside gameDir.
        QByteArray name = nameBytes;
        while (name.endsWith('\0')) name.chop(1);
        if (name.isEmpty() || name.startsWith('/') || name.contains('\\')) return std::nullopt;
        for (const char c : name) if (quint8(c) < 0x20) return std::nullopt;
        const QString path = QString::fromUtf8(name);
        // QDir::isAbsolutePath also rejects the Windows drive form ("C:/evil.bin"), which carries no
        // backslash, no leading '/' and no "..": QDir(gameDir).filePath() hands such a name back
        // UNCHANGED, so it would walk the verifier clean out of gameDir.
        if (QDir::isAbsolutePath(path)) return std::nullopt;
        if (path.contains(QStringLiteral("../")) || path == QStringLiteral("..")
            || path.endsWith(QStringLiteral("/.."))
            || path.contains(QChar(0xFFFD))) return std::nullopt; // 0xFFFD: not valid UTF-8
        e.path = path;
        e.size = qint64(fileSize);
        const quint8 low = quint8(type & 0xFF);
        e.isDir = (low == 0x04 || low == 0x12); // unpkg.cpp's two folder cases
        e.overwrite = (type & 0x80000000u) != 0;
        const quint64 dataOff = be64(table, o + 8); // subtraction form again: the sum can wrap
        if (!e.isDir && (dataOff > dataSize || fileSize > dataSize - dataOff)) return std::nullopt;
        out.append(e);
    }
    return out;
}

bool verifyInstalled(const QString&, const QVector<Entry>&) { return false; }  // Task 3

} // namespace Ps3Pkg
