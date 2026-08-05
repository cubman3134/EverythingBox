// Headless tests for the miximage compositor (src/core/Miximage, issue #90): the composited "game card"
// built from the art roles already cached — a screenshot base with box, logo and disc layered over it, so a
// library reads as one shelf even when scrape coverage is patchy. Prints MIXIMAGE-OK on success; MIXIMAGE-FAIL
// <what> and exits non-zero.
//
// The layout math is the thing under test, so the fixtures are SOLID-COLOUR squares (red screenshot, green
// box, blue logo, yellow disc) and the assertions read exact pixels at points this probe hardcodes from the
// documented geometry — NOT by calling back into Miximage, which would make the check a fixed point of the
// code it guards. If you move a layer in Miximage.cpp, move its sample point here and re-run under mutation.
//
// Canvas throughout is 1280x960 (the default 4:3). Sample points, as (x,y) in that canvas:
//   centre (640,432)  the screenshot base, clear of every overlay
//   logo   (640,96)   top-centre
//   box    (154,720)  lower-left
//   disc   (1126,787) lower-right
//   corner (1216,58)  top-right — the corner OPPOSITE the box, which the box must NOT reach
#include "AddonModels.h"
#include "AppPaths.h"
#include "MetaCache.h"
#include "Miximage.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, what) do { \
    if (!(cond)) { std::fprintf(stderr, "MIXIMAGE-FAIL %s (line %d)\n", what, __LINE__); ++failures; } \
} while (0)

static const QSize kCanvas(1280, 960);
static const QColor kRed(255, 0, 0), kGreen(0, 255, 0), kBlue(0, 0, 255), kYellow(255, 255, 0);
static const QRgb kBacking = qRgb(20, 22, 27); // Miximage's neutral fill (#14161B) when no screenshot

static QImage solid(const QColor& c, int side = 240)
{
    QImage img(side, side, QImage::Format_ARGB32);
    img.fill(c);
    return img;
}

// A channel-wise near-equality: PNG round-trips solids exactly, but a smooth-scaled solid can round a LSB, so
// a small tolerance keeps the assertion about WHICH layer landed, not about the scaler's last bit.
static bool nearRgb(QRgb got, const QColor& want, int tol = 12)
{
    return qAbs(qRed(got)   - want.red())   <= tol
        && qAbs(qGreen(got) - want.green()) <= tol
        && qAbs(qBlue(got)  - want.blue())  <= tol;
}
static bool nearRgb(QRgb got, QRgb want, int tol = 12)
{
    return qAbs(qRed(got) - qRed(want)) <= tol && qAbs(qGreen(got) - qGreen(want)) <= tol
        && qAbs(qBlue(got) - qBlue(want)) <= tol;
}

static QByteArray pngBytes(const QImage& img)
{
    QByteArray b;
    QBuffer buf(&b);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return b;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---------------------------------------------------------------- pure helpers
    CHECK(!Miximage::hasAnyInput(Miximage::Inputs{}), "no layers -> hasAnyInput is false");
    CHECK(Miximage::hasAnyInput(Miximage::Inputs{ {}, QStringLiteral("b.png"), {}, {} }),
          "one layer -> hasAnyInput is true");
    CHECK(Miximage::layoutName(Miximage::Inputs{}) == QStringLiteral("empty"),
          "the no-layer layout is named 'empty'");
    CHECK(Miximage::layoutName(Miximage::Inputs{ QStringLiteral("s"), QStringLiteral("b"),
                                                 QStringLiteral("l"), QStringLiteral("d") })
              == QStringLiteral("screenshot+box+logo+disc"),
          "the full combination names all four roles in order");
    CHECK(Miximage::layoutName(Miximage::Inputs{ QStringLiteral("s"), {}, QStringLiteral("l"), {} })
              == QStringLiteral("screenshot+logo"),
          "a partial combination names only the present roles");

    // ---------------------------------------------------------------- full combination: every layer lands
    {
        const QImage out = Miximage::composeImages(solid(kRed), solid(kGreen), solid(kBlue), solid(kYellow),
                                                   kCanvas);
        CHECK(out.size() == kCanvas, "the composite is exactly the requested canvas size");
        CHECK(nearRgb(out.pixel(640, 432), kRed),    "the screenshot base fills the centre");
        CHECK(nearRgb(out.pixel(640, 96),  kBlue),   "the logo lands top-centre");
        CHECK(nearRgb(out.pixel(154, 720), kGreen),  "the box lands lower-left");
        CHECK(nearRgb(out.pixel(1126, 787), kYellow), "the disc lands lower-right");
    }

    // ---------------------------------------------------------------- box lower-left, and the opposite corner
    // stays background. Screenshot-less so "background" is the known neutral fill, not another layer's colour —
    // this is the assertion that pins WHERE the box goes, not merely that it is drawn somewhere.
    {
        const QImage out = Miximage::composeImages(QImage(), solid(kGreen), QImage(), QImage(), kCanvas);
        CHECK(nearRgb(out.pixel(154, 720), kGreen), "box-only: the box is in the lower-left");
        CHECK(nearRgb(out.pixel(1216, 58), kBacking), "box-only: the opposite (top-right) corner is background");
        CHECK(nearRgb(out.pixel(640, 432), kBacking), "box-only: the centre is the neutral fill, not blank/box");
    }

    // ---------------------------------------------------------------- graceful degradation: each combination
    // is a valid, non-blank card with its present layers in place and its absent ones NOT invented.
    {
        // screenshot only -> the whole frame is the screenshot; no overlay colours anywhere we sample.
        const QImage s = Miximage::composeImages(solid(kRed), QImage(), QImage(), QImage(), kCanvas);
        CHECK(s.size() == kCanvas, "screenshot-only: canvas size");
        CHECK(nearRgb(s.pixel(640, 432), kRed), "screenshot-only: centre is the screenshot");
        CHECK(nearRgb(s.pixel(154, 720), kRed), "screenshot-only: lower-left is screenshot (no box invented)");
        CHECK(nearRgb(s.pixel(640, 96),  kRed), "screenshot-only: top-centre is screenshot (no logo invented)");

        // screenshot + logo -> logo present, box region still screenshot.
        const QImage sl = Miximage::composeImages(solid(kRed), QImage(), solid(kBlue), QImage(), kCanvas);
        CHECK(nearRgb(sl.pixel(640, 96),  kBlue), "screenshot+logo: the logo is present");
        CHECK(nearRgb(sl.pixel(154, 720), kRed),  "screenshot+logo: no box where none was given");

        // logo only -> logo over the neutral fill.
        const QImage l = Miximage::composeImages(QImage(), QImage(), solid(kBlue), QImage(), kCanvas);
        CHECK(nearRgb(l.pixel(640, 96),  kBlue),    "logo-only: the logo is present");
        CHECK(nearRgb(l.pixel(640, 432), kBacking), "logo-only: the rest is the neutral fill");

        // all empty -> still a valid, non-blank (neutral-filled) canvas rather than a crash or a null image.
        const QImage e = Miximage::composeImages(QImage(), QImage(), QImage(), QImage(), kCanvas);
        CHECK(e.size() == kCanvas, "all-empty: still a full canvas");
        CHECK(nearRgb(e.pixel(640, 432), kBacking), "all-empty: filled with the neutral backing, not blank");
    }

    // ---------------------------------------------------------------- determinism: same inputs -> same pixels
    {
        const QImage a = Miximage::composeImages(solid(kRed), solid(kGreen), solid(kBlue), solid(kYellow), kCanvas);
        const QImage b = Miximage::composeImages(solid(kRed), solid(kGreen), solid(kBlue), solid(kYellow), kCanvas);
        CHECK(a == b, "the compositor is deterministic (identical inputs -> identical output)");
    }

    // ---------------------------------------------------------------- ensureForKey: staleness + MetaCache role
    {
        const QString key = QStringLiteral("miximage-test-item");
        // Seed two input roles as real files in the item's folder (storeImage writes the bytes + records the
        // role, exactly as a download would). No network: we hand it PNG bytes we made here.
        MetaCache::storeImage(key, QStringLiteral("screenshot"), QStringLiteral("s.png"),
                              QStringLiteral("image/png"), pngBytes(solid(kRed)));
        MetaCache::storeImage(key, QStringLiteral("box"), QStringLiteral("b.png"),
                              QStringLiteral("image/png"), pngBytes(solid(kGreen)));
        CHECK(!MetaCache::imagePath(key, QStringLiteral("screenshot")).isEmpty(), "seed: screenshot cached");
        CHECK(!MetaCache::imagePath(key, QStringLiteral("box")).isEmpty(), "seed: box cached");

        const QString mixPath = Miximage::ensureForKey(key, kCanvas);
        CHECK(!mixPath.isEmpty(), "ensureForKey composites a card when inputs exist");
        CHECK(QFileInfo::exists(mixPath), "the composited card is written to disk");

        // The generated file has the box in the lower-left over the red screenshot.
        QImage gen;
        gen.load(mixPath);
        CHECK(gen.size() == kCanvas, "generated card: canvas size");
        CHECK(nearRgb(gen.pixel(640, 432), kRed),   "generated card: screenshot base");
        CHECK(nearRgb(gen.pixel(154, 720), kGreen), "generated card: box lower-left");

        // MetaCache surfaces it as the "miximage" art role so a theme's role:"miximage" resolves it.
        const MediaArt art = MetaCache::loadArt(key);
        CHECK(art.images.contains(QStringLiteral("miximage")), "loadArt surfaces the miximage role");
        CHECK(art.image(QStringLiteral("miximage")) == mixPath, "the miximage role points at the composite");

        // Staleness: a NEW input (disc) plus a composite older than EVERY input -> regenerate, new layer shows.
        // (Qt's setFileTime needs the file open. Backdate well past any input's mtime so the ONLY thing that can
        // trigger the rebuild is "an input is newer than the composite" — the comparison the mutation flips.)
        MetaCache::storeImage(key, QStringLiteral("disc"), QStringLiteral("d.png"),
                              QStringLiteral("image/png"), pngBytes(solid(kYellow)));
        { QFile f(mixPath); f.open(QIODevice::ReadWrite);
          f.setFileTime(QDateTime::currentDateTime().addDays(-1), QFileDevice::FileModificationTime); }
        const QString again = Miximage::ensureForKey(key, kCanvas);
        CHECK(again == mixPath, "regeneration reuses the same path");
        QImage gen2;
        gen2.load(again);
        CHECK(nearRgb(gen2.pixel(1126, 787), kYellow), "a changed input regenerates the card (disc now present)");

        // Freshness guard: a composite NEWER than every input is not rebuilt — its bytes are left untouched.
        { QFile f(again); f.open(QIODevice::ReadWrite);
          f.setFileTime(QDateTime::currentDateTime().addDays(1), QFileDevice::FileModificationTime); }
        const QByteArray before = [&] { QFile f(again); f.open(QIODevice::ReadOnly); return f.readAll(); }();
        const QString third = Miximage::ensureForKey(key, kCanvas);
        const QByteArray after = [&] { QFile f(third); f.open(QIODevice::ReadOnly); return f.readAll(); }();
        CHECK(before == after, "a fresh composite (newer than its inputs) is not regenerated");

        MetaCache::remove(key);
    }

    // ---------------------------------------------------------------- no inputs -> no card, and no blank one
    {
        const QString key = QStringLiteral("miximage-empty-item");
        CHECK(Miximage::ensureForKey(key, kCanvas).isEmpty(), "an item with no input art yields no card");
        CHECK(!MetaCache::loadArt(key).images.contains(QStringLiteral("miximage")),
              "…and no miximage role is fabricated for it");
        MetaCache::remove(key);
    }

    if (failures) { std::fprintf(stderr, "MIXIMAGE-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("MIXIMAGE-OK\n");
    return 0;
}
