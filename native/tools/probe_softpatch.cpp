// Headless check of ROM soft-patching (src/core/RomPatch) — the IPS / BPS / UPS sidecar-patch appliers and the
// launch-seam resolver that GameLauncher::prepareCore() calls (issue #128). The whole point of the feature is
// that a patch beside a ROM is applied to a *derived* file so the original is never touched, so that is what
// this probe pins, alongside byte-exact output for all three formats and the two ways a bad patch must be
// refused rather than silently launched.
//
// The fixtures are hand-built byte arrays constructed straight from the three format specs, and every expected
// output is computed by hand from the spec — NOT by running RomPatch::apply(). Deriving the "expected" bytes
// from the code under test would make the assertion a fixed point that passes no matter what the applier does
// (CONTRIBUTING.md: "fixtures that are fixed points of the function under test"). The embedded BPS/UPS CRC32
// footers were computed with an independent oracle (Python's zlib.crc32) and hardcoded, so a bug in RomPatch's
// own CRC32 cannot forge a matching checksum and hide behind it.
//
//   * IPS: a data record and an RLE-with-extension record, output computed by hand.
//   * UPS: a single-byte XOR change, size-equal, with correct source/target/patch CRC32s.
//   * BPS: SourceRead + TargetRead + SourceCopy (case 1) and TargetRead + TargetCopy (case 2) — all four
//     action commands — with correct CRC32 footers.
//   * mis-magic: a "*.ips"-shaped buffer whose content is not a patch is rejected (detectFormat == None,
//     apply == false) — a corrupt/foreign file must fail loudly, never launch as if valid.
//   * wrong source: a valid BPS applied to the wrong ROM is refused on the embedded source-checksum.
//   * xdelta3 (VCDIFF): the magic is detected, and the three things we do not implement — xdelta1, secondary
//     compression, a custom code table — are each refused with a message naming that specific cause, because
//     "unsupported" and "corrupt" are different facts and only one of them is actionable.
//   * xdelta3 decoding: ADD/RUN/COPY, an application header, a self-overlapping COPY, a second window reading
//     the target already produced, the last row of the default code table, and four ways a malformed window
//     must be refused. These fixtures are HAND-BUILT byte streams, not the output of a real encoder, so they
//     pin the decoder against the SPEC and cannot on their own prove it agrees with what xdelta3 emits.
//   * VCD_ADLER32 (win_indicator bit 0x04, an xdelta3 extension RFC 3284 does not define): the four extra
//     bytes are stepped over, the checksum is VERIFIED against the window's output, and a mismatch refuses
//     with a message saying the patch does not match this ROM. Since VCDIFF carries no source checksum, this
//     is the only evidence a patch built for a different dump ever produces.
//   * the seam: an .xdelta sidecar beside a ROM resolves to a patched derived file - the end-to-end proof
//     that isPatchExtension() and sidecarPatchFor()'s two separate extension lists agree.
//   * the seam: resolvePatchedRom() writes a derived file, returns its path, leaves the ROM byte-for-byte
//     unchanged (hashed before and after), and re-uses the cached result on a second call (idempotent); with
//     the setting off it is a no-op; a sidecar that fails to apply makes it return "" with an error.
//
// Prints SOFTPATCH-OK on success; any failure prints SOFTPATCH-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42) — the patched-ROM cache lives
// under it and is removed at exit. Fixture ROMs/patches are written under dataDir() too, never beside the exe.
#include "RomPatch.h"
#include "AppPaths.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "SOFTPATCH-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static QByteArray bytes(std::initializer_list<int> vs)
{
    QByteArray b;
    b.reserve(int(vs.size()));
    for (int v : vs) b.append(char(v & 0xFF));
    return b;
}

static QString sha(const QByteArray& b)
{
    return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha1).toHex());
}

// A minimal VCDIFF header: magic D6 C3 C4, version 00, then hdr_indicator.
static QByteArray vcdHeader(quint8 indicator)
{
    QByteArray h;
    h.append(char(0xD6)); h.append(char(0xC3)); h.append(char(0xC4)); h.append(char(0x00));
    h.append(char(indicator));
    return h;
}

// A VCDIFF integer (RFC 3284 §2): base-128, BIG-endian, high bit SET on every byte except the last. Written
// here from the spec, deliberately NOT by calling anything in RomPatch — a fixture built with the code under
// test is a fixed point that passes whatever that code does.
static void appendVcdInt(QByteArray& b, quint64 v)
{
    QByteArray tmp;
    tmp.append(char(v & 0x7F));
    v >>= 7;
    while (v) { tmp.append(char((v & 0x7F) | 0x80)); v >>= 7; }
    std::reverse(tmp.begin(), tmp.end());
    b.append(tmp);
}

// One VCDIFF window, laid out as RFC 3284 §4.3 states it:
//   win_indicator | [source segment length | source segment position] | delta encoding length | delta encoding
// and the delta encoding is:
//   target window length | delta indicator | data length | inst length | addr length | data | insts | addrs
// `adler` >= 0 emits the xdelta3 VCD_ADLER32 extension: four big-endian bytes of checksum sitting AFTER the
// three section lengths and BEFORE the three sections (the caller must also set win_indicator bit 0x04).
static QByteArray vcdWindow(quint8 winInd, quint64 segLen, quint64 segPos, quint64 tgtLen, quint8 deltaInd,
                            const QByteArray& data, const QByteArray& insts, const QByteArray& addrs,
                            qint64 adler = -1)
{
    QByteArray w;
    w.append(char(winInd));
    if (winInd & 0x03) { appendVcdInt(w, segLen); appendVcdInt(w, segPos); }

    QByteArray delta;
    appendVcdInt(delta, tgtLen);
    delta.append(char(deltaInd));
    appendVcdInt(delta, quint64(data.size()));
    appendVcdInt(delta, quint64(insts.size()));
    appendVcdInt(delta, quint64(addrs.size()));
    if (adler >= 0)
    {
        const quint32 a = quint32(adler);
        delta.append(char((a >> 24) & 0xFF));
        delta.append(char((a >> 16) & 0xFF));
        delta.append(char((a >> 8) & 0xFF));
        delta.append(char(a & 0xFF));
    }
    delta.append(data);
    delta.append(insts);
    delta.append(addrs);

    appendVcdInt(w, quint64(delta.size()));
    w.append(delta);
    return w;
}

// A whole one-window patch: header (with an optional application header, which every real xdelta3 patch has)
// followed by that single window.
static QByteArray vcdPatch(quint8 hdrInd, const QByteArray& appHeader, const QByteArray& window)
{
    QByteArray p = vcdHeader(hdrInd);
    if (hdrInd & 0x04) { appendVcdInt(p, quint64(appHeader.size())); p.append(appHeader); }
    p.append(window);
    return p;
}

static bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = f.write(data) == data.size();
    f.close();
    return ok;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using RomPatch::Format;

    // ---- 1. IPS: data record ------------------------------------------------------------------------------
    // "PATCH" | offset 0x000001 | len 0x0002 | AA BB | "EOF".  Overwrite bytes 1..2 of the source.
    {
        const QByteArray src   = bytes({0x00, 0x01, 0x02, 0x03, 0x04});
        const QByteArray patch = bytes({0x50,0x41,0x54,0x43,0x48, 0x00,0x00,0x01, 0x00,0x02, 0xAA,0xBB, 0x45,0x4F,0x46});
        const QByteArray want  = bytes({0x00, 0xAA, 0xBB, 0x03, 0x04}); // by hand from the spec
        CHECK(RomPatch::detectFormat(patch) == Format::Ips);
        QByteArray out;
        CHECK(RomPatch::apply(src, patch, out));
        CHECK(out == want);
        CHECK(src == bytes({0x00, 0x01, 0x02, 0x03, 0x04})); // source buffer untouched by apply()
    }

    // ---- 2. IPS: RLE record that extends the output -------------------------------------------------------
    // "PATCH" | offset 0x000000 | len 0x0000 (RLE) | run 0x0004 | value 0x99 | "EOF".  Source is 3 bytes; the
    // run writes four 0x99 at offset 0, extending the output to 4 bytes.
    {
        const QByteArray src   = bytes({0x10, 0x11, 0x12});
        const QByteArray patch = bytes({0x50,0x41,0x54,0x43,0x48, 0x00,0x00,0x00, 0x00,0x00, 0x00,0x04, 0x99, 0x45,0x4F,0x46});
        const QByteArray want  = bytes({0x99, 0x99, 0x99, 0x99});
        QByteArray out;
        CHECK(RomPatch::apply(src, patch, out));
        CHECK(out == want);
    }

    // ---- 3. UPS: one XOR change (source 0x01 -> target 0x05, delta 0x04), size-equal ---------------------
    // "UPS1" | srcSize 4 (0x84) | tgtSize 4 (0x84) | skip 1 (0x81) | xor 0x04, term 0x00 | srcCRC tgtCRC patchCRC.
    {
        const QByteArray src  = bytes({0x00, 0x01, 0x02, 0x03});
        const QByteArray want = bytes({0x00, 0x05, 0x02, 0x03});
        const QByteArray patch = bytes({
            0x55,0x50,0x53,0x31, 0x84, 0x84, 0x81, 0x04, 0x00,
            0x13,0x86,0xB9,0x8B,   // source CRC32 LE  (of 00 01 02 03)
            0xCF,0x2E,0xB0,0x8C,   // target CRC32 LE  (of 00 05 02 03)
            0x59,0x8D,0x93,0x54}); // patch  CRC32 LE
        CHECK(RomPatch::detectFormat(patch) == Format::Ups);
        QByteArray out;
        CHECK(RomPatch::apply(src, patch, out));
        CHECK(out == want);
    }

    // ---- 4. BPS case 1: SourceRead + TargetRead + SourceCopy ----------------------------------------------
    // "BPS1" | srcSize 4 | tgtSize 4 | meta 0 | SourceRead(1)=0x80, TargetRead(1)=0x81 0xFF, SourceCopy(2)=0x86 rel+2=0x84 | CRCs.
    // out[0]=src[0]=C0 ; out[1]=FF ; out[2..3]=src[2..3]=C2 C3.
    {
        const QByteArray src  = bytes({0xC0, 0xC1, 0xC2, 0xC3});
        const QByteArray want = bytes({0xC0, 0xFF, 0xC2, 0xC3});
        const QByteArray patch = bytes({
            0x42,0x50,0x53,0x31, 0x84, 0x84, 0x80, 0x80, 0x81,0xFF, 0x86,0x84,
            0xAB,0xEB,0xC5,0x3C,   // source CRC32 LE (of C0 C1 C2 C3)
            0x31,0x23,0x30,0x12,   // target CRC32 LE (of C0 FF C2 C3)
            0xE4,0x9E,0x97,0xBD}); // patch  CRC32 LE
        CHECK(RomPatch::detectFormat(patch) == Format::Bps);
        QByteArray out;
        CHECK(RomPatch::apply(src, patch, out));
        CHECK(out == want);

        // ---- 5. wrong source: the same valid BPS on a different ROM must be refused on the SOURCE CRC ----
        // The distinct-message assertion matters: a wrong ROM also fails the target-checksum backstop (it
        // produces different bytes), so merely checking "apply returned false" cannot tell whether the SOURCE
        // check fired at all — disabling it would still be caught by the target check downstream. Asserting the
        // error is the source-mismatch diagnosis ("does not match this ROM") proves the source check is what
        // refused it, which is the behaviour the issue requires: catch a wrong-ROM patch up front, by its
        // embedded source checksum, not incidentally after producing garbage.
        const QByteArray wrongSrc = bytes({0xDE, 0xAD, 0xBE, 0xEF}); // same length, so only the CRC can tell them apart
        QByteArray out2;
        QString werr;
        CHECK(!RomPatch::apply(wrongSrc, patch, out2, &werr));
        CHECK(werr.contains(QStringLiteral("does not match this ROM")));
    }

    // ---- 6. BPS case 2: TargetRead + TargetCopy (RLE-style self-copy) -------------------------------------
    // "BPS1" | srcSize 2 | tgtSize 3 | meta 0 | TargetRead(1)=0x81 0xAA, TargetCopy(2)=0x87 rel 0=0x80 | CRCs.
    // out[0]=AA ; out[1]=out[0]=AA ; out[2]=out[1]=AA.
    {
        const QByteArray src  = bytes({0x01, 0x02});
        const QByteArray want = bytes({0xAA, 0xAA, 0xAA});
        const QByteArray patch = bytes({
            0x42,0x50,0x53,0x31, 0x82, 0x83, 0x80, 0x81,0xAA, 0x87,0x80,
            0x92,0x42,0xCC,0xB6,   // source CRC32 LE (of 01 02)
            0x31,0x1F,0x45,0x49,   // target CRC32 LE (of AA AA AA)
            0x9B,0xCC,0x5D,0x98}); // patch  CRC32 LE
        QByteArray out;
        CHECK(RomPatch::apply(src, patch, out));
        CHECK(out == want);
    }

    // ---- 7. mis-magic: a file that is not a patch is rejected, not launched -------------------------------
    {
        const QByteArray notAPatch = bytes({0x4E, 0x4F, 0x50, 0x45, 0x21}); // "NOPE!"
        CHECK(RomPatch::detectFormat(notAPatch) == Format::None);
        QByteArray out;
        QString err;
        CHECK(!RomPatch::apply(bytes({0x00}), notAPatch, out, &err));
        CHECK(!err.isEmpty());
    }

    // ---- 8. The launch seam: resolvePatchedRom leaves the ORIGINAL untouched and patches a derived file ---
    // Use the IPS case-1 fixture on disk. The ROM keeps its .sfc extension; the patch is Game.ips beside it.
    {
        const QString root = AppPaths::dataDir() + QStringLiteral("/spfix");
        QDir().mkpath(root);
        const QString romPath   = root + QStringLiteral("/Game.sfc");
        const QString patchPath = root + QStringLiteral("/Game.ips");
        const QByteArray romBytes   = bytes({0x00, 0x01, 0x02, 0x03, 0x04});
        const QByteArray patchBytes = bytes({0x50,0x41,0x54,0x43,0x48, 0x00,0x00,0x01, 0x00,0x02, 0xAA,0xBB, 0x45,0x4F,0x46});
        const QByteArray wantPatched = bytes({0x00, 0xAA, 0xBB, 0x03, 0x04});
        CHECK(writeFile(romPath, romBytes));
        CHECK(writeFile(patchPath, patchBytes));

        Settings::setAutoApplyRomPatches(true);
        const QString romHashBefore = sha(romBytes);

        QString err;
        const QString launch = RomPatch::resolvePatchedRom(romPath, &err);
        CHECK(err.isEmpty());
        CHECK(!launch.isEmpty());
        CHECK(launch != romPath);                 // a DERIVED file, not the original
        CHECK(QFileInfo(launch).suffix() == QStringLiteral("sfc")); // extension preserved for system detection

        // The launched file holds the patched bytes...
        QFile lf(launch);
        CHECK(lf.open(QIODevice::ReadOnly));
        CHECK(lf.readAll() == wantPatched);
        lf.close();

        // ...and the ORIGINAL ROM on disk is byte-for-byte what it was.
        QFile rf(romPath);
        CHECK(rf.open(QIODevice::ReadOnly));
        const QByteArray romAfter = rf.readAll();
        rf.close();
        CHECK(sha(romAfter) == romHashBefore);
        CHECK(romAfter == romBytes);

        // Idempotent: a second call returns the same cached path without re-patching.
        QString err2;
        const QString launch2 = RomPatch::resolvePatchedRom(romPath, &err2);
        CHECK(err2.isEmpty());
        CHECK(launch2 == launch);

        // With the setting off, it is a no-op: the original path is returned, unpatched.
        Settings::setAutoApplyRomPatches(false);
        const QString launchOff = RomPatch::resolvePatchedRom(romPath, &err);
        CHECK(launchOff == romPath);
        Settings::setAutoApplyRomPatches(true);

        // No sidecar => the ROM path is returned unchanged.
        const QString lone = root + QStringLiteral("/Solo.sfc");
        CHECK(writeFile(lone, romBytes));
        const QString launchLone = RomPatch::resolvePatchedRom(lone, &err);
        CHECK(launchLone == lone);
        CHECK(err.isEmpty());
    }

    // ---- 9. The seam fails LOUDLY on a bad sidecar (does not fall through to the unpatched ROM) -----------
    // A file named .ips whose content is not a patch: resolvePatchedRom must return "" with an error, so the
    // launcher surfaces it instead of booting the ROM as if the patch had applied.
    {
        const QString root = AppPaths::dataDir() + QStringLiteral("/spbad");
        QDir().mkpath(root);
        const QString romPath   = root + QStringLiteral("/Bad.sfc");
        const QString patchPath = root + QStringLiteral("/Bad.ips");
        CHECK(writeFile(romPath, bytes({0x00, 0x01, 0x02})));
        CHECK(writeFile(patchPath, bytes({0x4E, 0x4F, 0x50, 0x45}))); // "NOPE" — not a patch

        Settings::setAutoApplyRomPatches(true);
        QString err;
        const QString launch = RomPatch::resolvePatchedRom(romPath, &err);
        CHECK(launch.isEmpty());       // refused
        CHECK(!err.isEmpty());         // with a message
        CHECK(launch != romPath);      // and NOT silently the unpatched ROM
    }

    // ---- 10. xdelta3 (VCDIFF): detection, and the three things we refuse BY NAME -------------------------
    // Detection is on the magic, as for the other three formats — never on the file's name.
    {
        CHECK(RomPatch::detectFormat(vcdHeader(0x00)) == Format::Xdelta);
    }

    // xdelta1 is a DIFFERENT container from VCDIFF, not a corrupt one. It must be refused by name: "we do not
    // support xdelta1" and "this file is corrupt" are different facts and only one of them tells the person
    // holding the file what to do next, so asserting merely that apply() said no would not pin the behaviour.
    {
        QByteArray x1("%XDZ004%");
        x1.append(QByteArray(32, '\0'));
        QByteArray out;
        QString err;
        CHECK(!RomPatch::apply(bytes({0x41, 0x41, 0x41, 0x41}), x1, out, &err));
        CHECK(err.contains(QStringLiteral("xdelta1"), Qt::CaseInsensitive));
        CHECK(out.isEmpty());                     // a refusal writes nothing
    }

    // A patch asking for a secondary compressor is refused with a message that says so — we implement neither
    // DJW nor LZMA, and half-parsing one would produce garbage that still looks like a patch.
    {
        QByteArray p = vcdHeader(0x01);           // VCD_DECOMPRESS
        p.append(char(0x01));                     // a compressor id
        QByteArray out;
        QString err;
        CHECK(!RomPatch::apply(bytes({0x41, 0x41, 0x41, 0x41}), p, out, &err));
        CHECK(err.contains(QStringLiteral("secondary"), Qt::CaseInsensitive));
        CHECK(out.isEmpty());
    }

    // A custom instruction code table is likewise out of scope, and refused rather than guessed at.
    {
        const QByteArray p = vcdHeader(0x02);     // VCD_CODETABLE
        QByteArray out;
        QString err;
        CHECK(!RomPatch::apply(bytes({0x41, 0x41, 0x41, 0x41}), p, out, &err));
        CHECK(err.contains(QStringLiteral("code table"), Qt::CaseInsensitive));
        CHECK(out.isEmpty());
    }

    // ---- 11. xdelta3 (VCDIFF) DECODING -------------------------------------------------------------------
    // Every stream below is built by hand from RFC 3284 and every expected output is worked out from the spec,
    // never by running RomPatch::apply(). They pin the decoder against the format as specified; they cannot
    // prove it agrees with what the real xdelta3 encoder emits — only real patches can do that.

    // The default code table (RFC 3284 §5.4) is GENERATED, not transcribed, and 256 is the one thing the
    // generator can get wrong silently. Q_ASSERT vanishes in Release, so assert the count here, at runtime: an
    // off-by-one would mis-decode every real patch while the hand-built fixtures below still passed.
    CHECK(RomPatch::vcdiffCodeTableEntries() == 256);

    // ADD + COPY-from-source, behind an application header (hdr_indicator 0x04 — what every real patch sets).
    // insts: code 5 = ADD size 4; code 19 = COPY size 0 mode 0, so its size follows as an integer.
    // addrs: mode 0 is an absolute address, 0 = the first byte of the source segment.
    {
        const QByteArray src("ABCDEFGH");
        QByteArray insts;
        insts.append(char(5));
        insts.append(char(19));
        appendVcdInt(insts, 4);
        QByteArray addrs;
        appendVcdInt(addrs, 0);
        const QByteArray p = vcdPatch(0x04, QByteArray("xdelta3"),
                                      vcdWindow(0x01, 8, 0, 8, 0x00, QByteArray("WXYZ"), insts, addrs));
        QByteArray out;
        QString err;
        CHECK(RomPatch::apply(src, p, out, &err));
        CHECK(out == QByteArray("WXYZABCD"));     // ADD "WXYZ" then the source's first four bytes
        CHECK(src == QByteArray("ABCDEFGH"));     // the source buffer is never touched
    }

    // RUN: code 0 is RUN with the size following. No source window at all (win_indicator 0).
    {
        QByteArray insts;
        insts.append(char(0));
        appendVcdInt(insts, 5);
        const QByteArray p = vcdPatch(0x00, QByteArray(),
                                      vcdWindow(0x00, 0, 0, 5, 0x00, QByteArray("Q"), insts, QByteArray()));
        QByteArray out;
        QString err;
        CHECK(RomPatch::apply(QByteArray("ignored"), p, out, &err));
        CHECK(out == QByteArray("QQQQQ"));
    }

    // A SELF-OVERLAPPING COPY: with no source window every address is inside the target window, so a COPY of
    // six bytes from address 0 while only two exist reads bytes it is itself writing. That is legal VCDIFF and
    // is how run-like sequences are encoded, so the decoder must copy BYTE BY BYTE — a memcpy/memmove would
    // read the untouched tail of the buffer instead and quietly produce "AB" + rubbish.
    // insts: code 3 = ADD size 2; code 22 = COPY size 6 mode 0 (19 = size 0, 20 = size 4, 21 = 5, 22 = 6).
    {
        QByteArray insts;
        insts.append(char(3));
        insts.append(char(22));
        QByteArray addrs;
        appendVcdInt(addrs, 0);
        const QByteArray p = vcdPatch(0x00, QByteArray(),
                                      vcdWindow(0x00, 0, 0, 8, 0x00, QByteArray("AB"), insts, addrs));
        QByteArray out;
        QString err;
        CHECK(RomPatch::apply(QByteArray(), p, out, &err));
        CHECK(out == QByteArray("ABABABAB"));
    }

    // The LAST row of the default code table: code 255 = COPY size 4 mode 8, then ADD size 1. Mode 8 is the
    // third "same" cache mode, whose address is a single raw byte indexing a table that starts all-zero — so
    // this also pins the address cache's same[] half. A generator that stopped one row short would fail here
    // while every fixture above still passed.
    {
        const QByteArray src("ABCDEFGH");
        QByteArray insts;
        insts.append(char(255));
        QByteArray addrs;
        addrs.append(char(0x00));                 // same[2 * 256 + 0] == 0 => address 0
        const QByteArray p = vcdPatch(0x00, QByteArray(),
                                      vcdWindow(0x01, 8, 0, 5, 0x00, QByteArray("Z"), insts, addrs));
        QByteArray out;
        QString err;
        CHECK(RomPatch::apply(src, p, out, &err));
        CHECK(out == QByteArray("ABCDZ"));
    }

    // TWO windows, the second copying from the TARGET already decoded (win_indicator 0x02). Pins that the
    // window loop advances exactly to the end of each window and concatenates their outputs.
    {
        QByteArray runInsts;
        runInsts.append(char(0));
        appendVcdInt(runInsts, 3);
        QByteArray copyInsts;
        copyInsts.append(char(19));               // COPY size 0 mode 0 => size follows
        appendVcdInt(copyInsts, 3);
        QByteArray addrs;
        appendVcdInt(addrs, 0);

        QByteArray p = vcdPatch(0x00, QByteArray(),
                                vcdWindow(0x00, 0, 0, 3, 0x00, QByteArray("M"), runInsts, QByteArray()));
        p.append(vcdWindow(0x02, 3, 0, 3, 0x00, QByteArray(), copyInsts, addrs));
        QByteArray out;
        QString err;
        CHECK(RomPatch::apply(QByteArray(), p, out, &err));
        CHECK(out == QByteArray("MMMMMM"));
    }

    // ---- the refusals: a malformed window must never yield a partial or plausible-looking ROM -------------
    {
        const QByteArray src("ABCDEFGH");
        QByteArray insts;
        insts.append(char(5));
        insts.append(char(19));
        appendVcdInt(insts, 4);
        QByteArray addrs;
        appendVcdInt(addrs, 0);

        // Truncated: the same good patch with its last three bytes cut off.
        {
            QByteArray p = vcdPatch(0x04, QByteArray("xdelta3"),
                                    vcdWindow(0x01, 8, 0, 8, 0x00, QByteArray("WXYZ"), insts, addrs));
            p.chop(3);
            QByteArray out;
            QString err;
            CHECK(!RomPatch::apply(src, p, out, &err));
            CHECK(!err.isEmpty());
            CHECK(out.isEmpty());
        }

        // A COPY address past everything that exists — the source segment is 8 bytes and nothing is produced.
        {
            QByteArray far;
            appendVcdInt(far, 100);
            const QByteArray p = vcdPatch(0x04, QByteArray("xdelta3"),
                                          vcdWindow(0x01, 8, 0, 8, 0x00, QByteArray("WXYZ"), insts, far));
            QByteArray out;
            QString err;
            CHECK(!RomPatch::apply(src, p, out, &err));
            CHECK(out.isEmpty());
        }

        // The window declares 9 bytes but its instructions produce 8. Short is a refusal, not a short file.
        {
            const QByteArray p = vcdPatch(0x04, QByteArray("xdelta3"),
                                          vcdWindow(0x01, 8, 0, 9, 0x00, QByteArray("WXYZ"), insts, addrs));
            QByteArray out;
            QString err;
            CHECK(!RomPatch::apply(src, p, out, &err));
            CHECK(out.isEmpty());
        }

        // delta_indicator != 0 asks for per-section secondary compression: refused, and the message says so.
        {
            const QByteArray p = vcdPatch(0x04, QByteArray("xdelta3"),
                                          vcdWindow(0x01, 8, 0, 8, 0x01, QByteArray("WXYZ"), insts, addrs));
            QByteArray out;
            QString err;
            CHECK(!RomPatch::apply(src, p, out, &err));
            CHECK(err.contains(QStringLiteral("secondary"), Qt::CaseInsensitive));
            CHECK(out.isEmpty());
        }

        // Trailing bytes after the last window: the stream must end exactly where the windows do.
        {
            QByteArray p = vcdPatch(0x04, QByteArray("xdelta3"),
                                    vcdWindow(0x01, 8, 0, 8, 0x00, QByteArray("WXYZ"), insts, addrs));
            p.append(char(0x00));
            QByteArray out;
            QString err;
            CHECK(!RomPatch::apply(src, p, out, &err));
            CHECK(out.isEmpty());
        }
    }

    // ---- 12. Sidecar extensions: the names a patch beside a ROM may carry ---------------------------------
    {
        CHECK(RomPatch::isPatchExtension(QStringLiteral("xdelta")));
        CHECK(RomPatch::isPatchExtension(QStringLiteral("xdelta3")));
        CHECK(RomPatch::isPatchExtension(QStringLiteral("vcdiff")));
        CHECK(!RomPatch::isPatchExtension(QStringLiteral("zip")));
    }

    // ---- 13. VCD_ADLER32: the four bytes RFC 3284 does not mention, and the only check VCDIFF gives us ----
    // Every window of every real xdelta3 patch sets win_indicator bit 0x04, which inserts a big-endian adler32
    // of that window's target output between the section lengths and the sections. A decoder written from the
    // RFC alone starts the data section four bytes early on every real patch. It is also the one integrity
    // guarantee the format offers: VCDIFF embeds no SOURCE checksum, so a patch built for another dump applies
    // to any ROM merely long enough - the wrong RESULT is where the mistake becomes visible, and catching it
    // there is what actually protects the person holding the ROM.
    {
        const QByteArray src("ABCDEFGH");
        QByteArray insts;
        insts.append(char(5));                          // code 5 = ADD size 4
        insts.append(char(19));                         // code 19 = COPY size-follows, mode 0
        appendVcdInt(insts, 4);
        QByteArray addrs;
        appendVcdInt(addrs, 0);                         // mode 0: absolute address 0
        // adler32("WXYZABCD"), computed with an INDEPENDENT oracle (Python zlib.adler32) and hardcoded, so a
        // bug in RomPatch's own adler32 cannot forge a checksum that agrees with itself.
        const quint32 kGood = 0x0B94026Du;

        // A window carrying a CORRECT checksum applies, and produces the same bytes as the un-checksummed
        // window in section 11.
        {
            const QByteArray p = vcdPatch(0x04, QByteArray("xdelta3"),
                                          vcdWindow(0x05, 8, 0, 8, 0x00, QByteArray("WXYZ"), insts, addrs, kGood));
            QByteArray out;
            QString err;
            CHECK(RomPatch::apply(src, p, out, &err));
            CHECK(err.isEmpty());
            CHECK(out == QByteArray("WXYZABCD"));
        }

        // The same window with a checksum that disagrees with what it produced: refused, and the message says
        // the patch does not match this ROM - that is the actionable fact, not "corrupt file".
        {
            const QByteArray p = vcdPatch(0x04, QByteArray("xdelta3"),
                                          vcdWindow(0x05, 8, 0, 8, 0x00, QByteArray("WXYZ"), insts, addrs,
                                                    kGood ^ 1u));
            QByteArray out;
            QString err;
            CHECK(!RomPatch::apply(src, p, out, &err));
            CHECK(err.contains(QStringLiteral("does not match this ROM"), Qt::CaseInsensitive));
            CHECK(out.isEmpty());
        }

        // A window that claims the checksum but has no room for it is refused, not read past.
        {
            QByteArray delta;
            appendVcdInt(delta, 1);                     // target window length
            delta.append(char(0x00));                   // delta_indicator
            appendVcdInt(delta, 0);                     // data / inst / addr lengths, all empty: nothing left
            appendVcdInt(delta, 0);
            appendVcdInt(delta, 0);
            QByteArray w;
            w.append(char(0x05));                       // VCD_SOURCE | VCD_ADLER32
            appendVcdInt(w, 8);
            appendVcdInt(w, 0);
            appendVcdInt(w, quint64(delta.size()));
            w.append(delta);
            QByteArray out;
            QString err;
            CHECK(!RomPatch::apply(src, vcdPatch(0x00, QByteArray(), w), out, &err));
            CHECK(err.contains(QStringLiteral("adler32"), Qt::CaseInsensitive));
            CHECK(out.isEmpty());
        }
    }

    // ---- 14. The seam finds an .xdelta sidecar beside a ROM ----------------------------------------------
    // resolvePatchedRom() reaches an xdelta3 patch only if BOTH extension lists learned the new suffixes:
    // isPatchExtension() and sidecarPatchFor()'s own literal list. Updating one and not the other is the
    // classic miss here, and it leaves the feature unreachable while every unit-level check still passes.
    {
        const QString root = AppPaths::dataDir() + QStringLiteral("/spxd");
        QDir().mkpath(root);
        const QString romPath   = root + QStringLiteral("/Game.nds");
        const QString patchPath = root + QStringLiteral("/Game.xdelta");
        const QByteArray romBytes("ABCDEFGH");

        QByteArray insts;
        insts.append(char(5));
        insts.append(char(19));
        appendVcdInt(insts, 4);
        QByteArray addrs;
        appendVcdInt(addrs, 0);
        const QByteArray patchBytes = vcdPatch(0x04, QByteArray("xdelta3"),
                                               vcdWindow(0x05, 8, 0, 8, 0x00, QByteArray("WXYZ"), insts, addrs,
                                                         0x0B94026D));
        CHECK(writeFile(romPath, romBytes));
        CHECK(writeFile(patchPath, patchBytes));
        Settings::setAutoApplyRomPatches(true);

        QString err;
        const QString launch = RomPatch::resolvePatchedRom(romPath, &err);
        CHECK(err.isEmpty());
        CHECK(!launch.isEmpty());
        CHECK(launch != romPath);                                     // a DERIVED file
        CHECK(QFileInfo(launch).suffix() == QStringLiteral("nds"));   // extension preserved

        QFile lf(launch);
        CHECK(lf.open(QIODevice::ReadOnly));
        CHECK(lf.readAll() == QByteArray("WXYZABCD"));
        lf.close();

        QFile rf(romPath);                                            // the ROM itself is untouched
        CHECK(rf.open(QIODevice::ReadOnly));
        CHECK(rf.readAll() == romBytes);
        rf.close();
    }

    if (failures == 0) std::printf("SOFTPATCH-OK\n");
    else std::fprintf(stderr, "SOFTPATCH: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
