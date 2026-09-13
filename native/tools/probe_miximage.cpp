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

        // LRU-touch guard (the shipped perf bug): MetaCache::imagePath bumps a served input's mtime once per
        // run, which used to flunk the mtime staleness check and RE-COMPOSITE every item on its first display
        // each run — 150-418ms of image work per nav.select on the GUI thread. Staleness is now judged by an
        // input identity stamp (path+size), so an mtime-only bump must NOT rebuild. Mutation-kill: revert to
        // mtime comparison and this fails.
        { QFile f(MetaCache::imagePath(key, QStringLiteral("screenshot"))); f.open(QIODevice::ReadWrite);
          f.setFileTime(QDateTime::currentDateTime().addDays(2), QFileDevice::FileModificationTime); }
        const QByteArray before2 = [&] { QFile f(third); f.open(QIODevice::ReadOnly); return f.readAll(); }();
        const QString fourth = Miximage::ensureForKey(key, kCanvas);
        const QByteArray after2 = [&] { QFile f(fourth); f.open(QIODevice::ReadOnly); return f.readAll(); }();
        CHECK(before2 == after2, "an mtime-only bump (LRU touch) does NOT re-composite the card");

        MetaCache::remove(key);
    }

    // ---------------------------------------------------------------- #387: a broken input never reaches a card
    // An input role can be a 200 error page stored as box.jpg by a build before #387. compose() skips a layer that
    // will not decode, so no broken PIXELS land - but the card's identity stamp then names the page, the page is
    // "already cached" for good, and the real box is never fetched: the card is built from a broken input for ever.
    // The inputs are read back through MetaCache's bytes check, so the page is removed, the card is rebuilt from
    // the good inputs, and the real box - once it lands - joins it.
    {
        const QByteArray page = QByteArrayLiteral("<!DOCTYPE html>\n<html><head><title>502 Bad Gateway</title></head>"
                                                  "<body><h1>502 Bad Gateway</h1></body></html>\n");
        auto readAll = [](const QString& p) { QFile f(p); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
        auto boxRecord = [](const QString& key) {
            return MetaCache::load(key).value(QStringLiteral("images")).toObject().value(QStringLiteral("box")).toString();
        };
        auto boxFiles = [](const QString& key) {
            return int(QDir(MetaCache::dirFor(key)).entryList({ QStringLiteral("box.*") }, QDir::Files).size());
        };

        // (a) a fresh item: good screenshot, box stored as a page.
        {
            const QString key = QStringLiteral("miximage-387-fresh");
            MetaCache::remove(key);
            MetaCache::storeImage(key, QStringLiteral("screenshot"), QStringLiteral("s.png"), QStringLiteral("image/png"),
                                  pngBytes(solid(kRed)));
            MetaCache::storeImage(key, QStringLiteral("box"), QStringLiteral("b.jpg"), QStringLiteral("image/jpeg"), page);
            const QString pagePath = MetaCache::dirFor(key) + QLatin1Char('/') + boxRecord(key);
            CHECK(boxRecord(key) == QStringLiteral("box.jpg") && readAll(pagePath) == page, "387 fresh: fixture - the page is stored as box");

            const QString card = Miximage::ensureForKey(key, kCanvas);
            CHECK(!card.isEmpty(), "387 fresh: a card is still made from the good screenshot");
            CHECK(boxFiles(key) == 0 && boxRecord(key).isEmpty(), "387 fresh: the page box is removed, file and entry");
            CHECK(!readAll(MetaCache::dirFor(key) + QStringLiteral("/miximage.stamp")).contains("/box."),
                  "387 fresh: the card's stamp does not name the page");
            QImage c1; c1.load(card);
            CHECK(nearRgb(c1.pixel(154, 720), kRed), "387 fresh: no box layer in the card (the screenshot shows there)");

            // The real box arrives (storeImage is the persist path cacheImage ends in): the card picks it up.
            MetaCache::storeImage(key, QStringLiteral("box"), QStringLiteral("b.png"), QStringLiteral("image/png"),
                                  pngBytes(solid(kGreen)));
            CHECK(boxRecord(key) == QStringLiteral("box.png"), "387 fresh: the real box is stored once the page is gone");
            const QString again = Miximage::ensureForKey(key, kCanvas);
            QImage c2; c2.load(again);
            CHECK(nearRgb(c2.pixel(154, 720), kGreen), "387 fresh: the card is rebuilt with the real box");
            CHECK(nearRgb(c2.pixel(640, 432), kRed), "387 fresh: ...over the same screenshot");
            MetaCache::remove(key);
        }

        // (b) a card an EARLIER build made while the page was there - its stamp names the page, so as far as the
        // staleness check goes it is "fresh". It must not be kept.
        {
            const QString key = QStringLiteral("miximage-387-old-card");
            MetaCache::remove(key);
            MetaCache::storeImage(key, QStringLiteral("screenshot"), QStringLiteral("s.png"), QStringLiteral("image/png"),
                                  pngBytes(solid(kRed)));
            MetaCache::storeImage(key, QStringLiteral("box"), QStringLiteral("b.jpg"), QStringLiteral("image/jpeg"), page);
            Miximage::ComposePlan old;
            old.viable = true;
            old.in.screenshot = MetaCache::imagePath(key, QStringLiteral("screenshot"));
            old.in.box = MetaCache::imagePath(key, QStringLiteral("box"));
            old.outPath = MetaCache::dirFor(key) + QStringLiteral("/miximage.png");
            old.stampPath = MetaCache::dirFor(key) + QStringLiteral("/miximage.stamp");
            // The identity planForKey writes (Miximage.h: each input's path + byte size), as that build computed it.
            for (const QString& p : { old.in.screenshot, old.in.box, old.in.logo, old.in.disc })
                old.identity += (p.isEmpty() ? QByteArray("-")
                                             : p.toUtf8() + ':' + QByteArray::number(QFileInfo(p).size())) + '\n';
            CHECK(!old.in.box.isEmpty() && Miximage::composeAndSave(old, kCanvas), "387 old card: fixture - the old card is made");
            MetaCache::recordLocalImage(key, QStringLiteral("miximage"), QStringLiteral("miximage.png"));
            CHECK(readAll(old.stampPath) == old.identity && old.identity.contains("box.jpg"),
                  "387 old card: fixture - its stamp names the page");

            const QString card = Miximage::ensureForKey(key, kCanvas);
            CHECK(card == old.outPath, "387 old card: the card keeps its path");
            CHECK(!readAll(old.stampPath).contains("/box."), "387 old card: it is rebuilt - the new stamp no longer names the page");
            CHECK(boxFiles(key) == 0 && boxRecord(key).isEmpty(), "387 old card: the page box is removed");
            MetaCache::storeImage(key, QStringLiteral("box"), QStringLiteral("b.png"), QStringLiteral("image/png"),
                                  pngBytes(solid(kGreen)));
            QImage c; c.load(Miximage::ensureForKey(key, kCanvas));
            CHECK(nearRgb(c.pixel(154, 720), kGreen), "387 old card: the real box joins the rebuilt card");
            MetaCache::remove(key);
        }

        // (c) the page is the ONLY input: no card, no role, and the page is gone.
        {
            const QString key = QStringLiteral("miximage-387-only-page");
            MetaCache::remove(key);
            MetaCache::storeImage(key, QStringLiteral("box"), QStringLiteral("b.jpg"), QStringLiteral("image/jpeg"), page);
            CHECK(!boxRecord(key).isEmpty(), "387 only page: fixture - the page is stored");
            CHECK(Miximage::ensureForKey(key, kCanvas).isEmpty(), "387 only page: no card is made");
            CHECK(!QFileInfo::exists(MetaCache::dirFor(key) + QStringLiteral("/miximage.png")), "387 only page: no card file");
            CHECK(!MetaCache::loadArt(key).images.contains(QStringLiteral("miximage")), "387 only page: no miximage role");
            CHECK(boxFiles(key) == 0 && boxRecord(key).isEmpty(), "387 only page: the page box is removed");
            MetaCache::remove(key);
        }

        // (d) genuine inputs are never removed by the read-back, and the card is not rebuilt for reading them (its
        // bytes stay put across repeated asks). PNG only: the one format Qt both writes and reads with no plugin on
        // every runner; probe_meta pins every stored format against the same check.
        {
            const QString key = QStringLiteral("miximage-387-genuine");
            MetaCache::remove(key);
            const QByteArray shot = pngBytes(solid(kRed)), box = pngBytes(solid(kGreen)), logo = pngBytes(solid(kBlue));
            MetaCache::storeImage(key, QStringLiteral("screenshot"), QStringLiteral("s.png"), QStringLiteral("image/png"), shot);
            MetaCache::storeImage(key, QStringLiteral("box"), QStringLiteral("b.png"), QStringLiteral("image/png"), box);
            MetaCache::storeImage(key, QStringLiteral("logo"), QStringLiteral("l.png"), QStringLiteral("image/png"), logo);
            const QString card = Miximage::ensureForKey(key, kCanvas);
            const QByteArray first = readAll(card);
            for (int i = 0; i < 3; ++i) CHECK(Miximage::ensureForKey(key, kCanvas) == card, "387 genuine: same card");
            CHECK(readAll(card) == first, "387 genuine: the card is not rebuilt for being read");
            CHECK(readAll(MetaCache::imagePath(key, QStringLiteral("box"))) == box
                      && readAll(MetaCache::imagePath(key, QStringLiteral("logo"))) == logo
                      && readAll(MetaCache::imagePath(key, QStringLiteral("screenshot"))) == shot,
                  "387 genuine: every input is still stored, bytes unchanged");
            QImage c; c.load(card);
            CHECK(nearRgb(c.pixel(154, 720), kGreen) && nearRgb(c.pixel(640, 96), kBlue),
                  "387 genuine: box and logo are in the card");
            MetaCache::remove(key);
        }
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
