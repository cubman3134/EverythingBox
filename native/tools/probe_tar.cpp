// probe_tar — the pure heart of the #144 CBT reader (native/src/comic/Tar.h) and the natural page-order
// collation the CB7/CBT readers share (native/src/comic/ComicPageOrder.h). Both are header-only and QtCore-
// only, so both are asserted headless here; opening a real .cb7/.cbt on screen rides ComicView + the same
// SevenZip path ROMs already use and is not headlessly drivable (that is verified by hand).
//
// The fixture is independent of the code under test. This file HAND-LAYS the 512-byte tar blocks — a byte
// placer that shares no logic with Tar.h's parser — and every expected value (offsets 512/2048/3584, sizes
// 12/600/5, the "sub/page2.png" prefixed name) is a literal constant written here, never a value read back
// from listEntries(). A fixture that was the parser's own output would prove nothing.
//
// Prints TAR-OK on success; any failure prints TAR-FAIL <cond> (line) and exits non-zero.
#include "Tar.h"
#include "ComicPageOrder.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QCollator>
#include <algorithm>
#include <cstring>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "TAR-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ---- Fixture builder (independent of Tar.h — it writes raw bytes; Tar.h reads them) -----------------------

// A tar size field: 11 zero-padded octal digits then a NUL, in a 12-byte field. This is a plain encoder, not
// the inverse of Tar.h's octal() decoder.
static void writeOctal(char* p, qint64 value)
{
    for (int i = 10; i >= 0; --i) { p[i] = char('0' + int(value & 7)); value >>= 3; }
    p[11] = '\0';
}

// Lay a single 512-byte ustar/old-tar header at buf[off]. buf is pre-zeroed, so unset fields stay zero.
static void emitHeader(QByteArray& buf, qint64 off, const QString& name, qint64 size,
                       char typeflag, bool ustar, const QString& prefix)
{
    char* h = buf.data() + off;
    const QByteArray n = name.toUtf8();
    std::memcpy(h, n.constData(), size_t(qMin<qint64>(n.size(), 100)));
    writeOctal(h + 124, size);
    h[156] = typeflag;
    if (ustar)
    {
        std::memcpy(h + 257, "ustar", 5); // magic; version bytes left as the reader ignores them
        h[263] = '0'; h[264] = '0';
    }
    if (!prefix.isEmpty())
    {
        const QByteArray pf = prefix.toUtf8();
        std::memcpy(h + 345, pf.constData(), size_t(qMin<qint64>(pf.size(), 155)));
    }
}

static void putData(QByteArray& buf, qint64 off, const QByteArray& content)
{
    std::memcpy(buf.data() + off, content.constData(), size_t(content.size()));
}

int main()
{
    // Page contents, distinctive and of exact known length. contentC spans two 512-byte data blocks (600 > 512),
    // which is what makes the offset of the *following* entry a real test of the padding math.
    const QByteArray contentA = QByteArrayLiteral("JPEG-PAGE-1!");            // 12 bytes
    QByteArray contentC(600, '\0');
    for (int i = 0; i < contentC.size(); ++i) contentC[i] = char('0' + (i % 10));
    const QByteArray contentD = QByteArrayLiteral("GIF10");                   // 5 bytes

    // Layout (all offsets hand-computed):
    //   [0]    header page1.jpg (ustar, regular)        data @ 512  (12 -> pad 512) -> next 1024
    //   [1024] header sub       (ustar, DIRECTORY)      size 0                       -> next 1536
    //   [1536] header page2.png (ustar, prefix "sub")   data @ 2048 (600 -> pad 1024)-> next 3072
    //   [3072] header page10.jpg (OLD format, '\0')     data @ 3584 (5 -> pad 512)   -> next 4096
    //   [4096] zero block  \ two-block terminator
    //   [4608] zero block  /
    //   [5120] header afterEnd.jpg (past terminator — MUST NOT be listed) data @ 5632
    const qint64 TOTAL = 6144;
    QByteArray tar(TOTAL, '\0');
    emitHeader(tar, 0,    QStringLiteral("page1.jpg"),  12,  '0',  true,  QString());
    putData(tar, 512, contentA);
    emitHeader(tar, 1024, QStringLiteral("sub"),        0,   '5',  true,  QString());   // directory: skipped
    emitHeader(tar, 1536, QStringLiteral("page2.png"),  600, '0',  true,  QStringLiteral("sub"));
    putData(tar, 2048, contentC);
    emitHeader(tar, 3072, QStringLiteral("page10.jpg"), 5,   '\0', false, QString());   // old format, '\0'=regular
    putData(tar, 3584, contentD);
    emitHeader(tar, 5120, QStringLiteral("afterEnd.jpg"), 3, '0',  true,  QString());
    putData(tar, 5632, QByteArrayLiteral("END"));

    // ---- 1. listEntries: exactly the three regular files, directory skipped, terminator stops the walk ------
    {
        const QVector<Tar::TarEntry> es = Tar::listEntries(tar);
        CHECK(es.size() == 3); // page1, sub/page2, page10 — NOT the directory, NOT afterEnd past the zero blocks

        // No entry may be the directory or the post-terminator file (independent constants, not read from es).
        for (const Tar::TarEntry& e : es)
        {
            CHECK(e.name != QStringLiteral("sub"));           // bare directory name never surfaces
            CHECK(e.name != QStringLiteral("afterEnd.jpg"));  // the walk stopped at the two zero blocks
        }

        if (es.size() == 3)
        {
            CHECK(es[0].name == QStringLiteral("page1.jpg"));
            CHECK(es[0].size == 12);
            CHECK(es[0].dataOffset == 512);

            CHECK(es[1].name == QStringLiteral("sub/page2.png")); // ustar prefix joined with '/'
            CHECK(es[1].size == 600);
            CHECK(es[1].dataOffset == 2048);

            CHECK(es[2].name == QStringLiteral("page10.jpg"));    // old-format header tolerated
            CHECK(es[2].size == 5);
            CHECK(es[2].dataOffset == 3584);

            // ---- 2. extractEntry returns the exact member bytes -----------------------------------------
            CHECK(Tar::extractEntry(tar, es[0]) == contentA);
            CHECK(Tar::extractEntry(tar, es[1]) == contentC);
            CHECK(Tar::extractEntry(tar, es[2]) == contentD);
        }
    }

    // ---- 3. Truncated tar degrades without throwing -----------------------------------------------------
    // Chop so page10's declared 5 bytes have only 2 present. listEntries must still parse the intact headers
    // and NOT crash; extractEntry must return the surviving bytes, clamped, never past the buffer.
    // (No-throw is a deliberate tripwire — reaching the assertions below at all is the proof it did not throw;
    //  the byte compare additionally pins that the 2 bytes are page10's real prefix, not garbage.)
    {
        const QByteArray trunc = tar.left(3586); // 3584 (page10 data start) + 2 bytes
        const QVector<Tar::TarEntry> es = Tar::listEntries(trunc);
        CHECK(es.size() == 3); // three headers are intact; only the last member's DATA is short
        if (es.size() == 3)
        {
            CHECK(es[2].name == QStringLiteral("page10.jpg"));
            CHECK(es[2].size == 5);                 // the header still DECLARES 5
            const QByteArray got = Tar::extractEntry(trunc, es[2]);
            CHECK(got.size() == 2);                 // but only 2 survived the truncation
            CHECK(got == contentD.left(2));
        }
    }

    // ---- 4. Natural page order: page2 before page10 (numeric-aware, case-insensitive) -------------------
    {
        const QCollator coll = ComicPages::collator();
        CHECK(ComicPages::lessThan(coll, QStringLiteral("page2.png"),  QStringLiteral("page10.jpg")) == true);
        CHECK(ComicPages::lessThan(coll, QStringLiteral("page10.jpg"), QStringLiteral("page2.png"))  == false);

        QStringList names = { QStringLiteral("page10.jpg"), QStringLiteral("Page2.png"),
                              QStringLiteral("page1.jpg"),  QStringLiteral("page20.jpg") };
        std::sort(names.begin(), names.end(),
                  [&coll](const QString& a, const QString& b) { return ComicPages::lessThan(coll, a, b); });
        const QStringList expected = { QStringLiteral("page1.jpg"), QStringLiteral("Page2.png"),
                                       QStringLiteral("page10.jpg"), QStringLiteral("page20.jpg") };
        CHECK(names == expected); // 1, 2, 10, 20 — not lexical 1, 10, 2, 20; case-insensitive keeps Page2 second
    }

    if (failures == 0) { std::printf("TAR-OK\n"); return 0; }
    std::fprintf(stderr, "TAR-FAIL total=%d\n", failures);
    return 1;
}
