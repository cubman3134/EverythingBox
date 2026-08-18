#include "Miximage.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "MetaCache.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QPainter>
#include <QSettings>
#include <algorithm>

namespace
{
// The layout, as fractions of the canvas. Anchored per role and INDEPENDENT of which other layers exist —
// that fixed geometry is the degradation guarantee. probe_miximage hardcodes the sample points these imply
// (rather than calling back into this file) so the assertions cannot become a fixed point of the code they
// test; if you move a layer here, move its sample point there and re-run the probe under mutation.
constexpr double kMarginX   = 0.03; // side / corner margins for the overlays
constexpr double kMarginY   = 0.03;

constexpr double kLogoMaxW  = 0.60; // title logo: top-centre
constexpr double kLogoMaxH  = 0.18;
constexpr double kLogoTop   = 0.04;

constexpr double kBoxMaxW   = 0.42; // box art: lower-left
constexpr double kBoxMaxH   = 0.55;

constexpr double kDiscMaxW  = 0.38; // physical media: lower-right
constexpr double kDiscMaxH  = 0.38;

// The neutral backing painted when there is no screenshot, so a box/logo-only card is a card, not a void.
const QColor kBacking(20, 22, 27); // #14161B

// Scale `src` to fit inside (maxW x maxH) preserving aspect; a null src stays null (absent layer).
QImage fitted(const QImage& src, int maxW, int maxH)
{
    if (src.isNull() || maxW <= 0 || maxH <= 0) return {};
    return src.scaled(maxW, maxH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}
} // namespace

bool Miximage::hasAnyInput(const Inputs& in)
{
    return !in.screenshot.isEmpty() || !in.box.isEmpty() || !in.logo.isEmpty() || !in.disc.isEmpty();
}

QString Miximage::layoutName(const Inputs& in)
{
    QStringList parts;
    if (!in.screenshot.isEmpty()) parts << QStringLiteral("screenshot");
    if (!in.box.isEmpty())        parts << QStringLiteral("box");
    if (!in.logo.isEmpty())       parts << QStringLiteral("logo");
    if (!in.disc.isEmpty())       parts << QStringLiteral("disc");
    return parts.isEmpty() ? QStringLiteral("empty") : parts.join(QLatin1Char('+'));
}

QSize Miximage::defaultCanvas()
{
    QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile), QSettings::IniFormat);
    int h = s.value(QStringLiteral("miximage/height"), 960).toInt();
    h = std::clamp(h, 240, 4320);          // sane bounds; the default (960) is the ES-DE "1x" height
    const int w = (h * 4) / 3;             // 4:3, the shape a screenshot base wants
    return QSize(w, h);
}

QImage Miximage::composeImages(const QImage& screenshot, const QImage& box, const QImage& logo,
                               const QImage& disc, QSize canvas)
{
    if (canvas.width() <= 0 || canvas.height() <= 0) return {};
    const int W = canvas.width();
    const int H = canvas.height();

    QImage out(canvas, QImage::Format_ARGB32);
    out.fill(kBacking); // never a blank frame, even with no screenshot

    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Base: the screenshot, scaled to COVER the whole canvas (crop the overflow, centred). A missing
    // screenshot leaves the neutral backing showing through.
    if (!screenshot.isNull())
    {
        const QImage s = screenshot.scaled(canvas, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const int cx = (s.width()  - W) / 2;
        const int cy = (s.height() - H) / 2;
        p.drawImage(QRect(0, 0, W, H), s, QRect(cx, cy, W, H));
    }

    const int mx = int(kMarginX * W);
    const int my = int(kMarginY * H);

    // Box art: lower-left.
    if (const QImage b = fitted(box, int(kBoxMaxW * W), int(kBoxMaxH * H)); !b.isNull())
        p.drawImage(mx, H - my - b.height(), b);

    // Physical media (disc / cart): lower-right.
    if (const QImage d = fitted(disc, int(kDiscMaxW * W), int(kDiscMaxH * H)); !d.isNull())
        p.drawImage(W - mx - d.width(), H - my - d.height(), d);

    // Title logo: top-centre. Drawn last so it reads over the screenshot's top edge.
    if (const QImage l = fitted(logo, int(kLogoMaxW * W), int(kLogoMaxH * H)); !l.isNull())
        p.drawImage((W - l.width()) / 2, int(kLogoTop * H), l);

    p.end();
    return out;
}

QImage Miximage::compose(const Inputs& in, QSize canvas)
{
    // A path that fails to load becomes a null QImage -> an absent layer, so a truncated download degrades
    // like a missing role instead of aborting the card.
    auto loadOr = [](const QString& path) -> QImage {
        if (path.isEmpty()) return {};
        QImage img;
        img.load(path);
        return img; // null on failure
    };
    const QImage s = loadOr(in.screenshot);
    const QImage b = loadOr(in.box);
    const QImage l = loadOr(in.logo);
    const QImage d = loadOr(in.disc);
    if (s.isNull() && b.isNull() && l.isNull() && d.isNull()) return {}; // nothing loaded -> no card
    return composeImages(s, b, l, d, canvas);
}

Miximage::ComposePlan Miximage::planForKey(const QString& key)
{
    ComposePlan plan;
    if (key.isEmpty()) return plan;

    // The cached input roles. logo falls back to clearlogo, disc to cart — the conventional aliases a
    // provider may have used (THEME_FORMAT.md lists them). These are all LOCAL files (imagePath returns ""
    // for an uncached role). MetaCache has unguarded statics, so this half MUST stay on the GUI thread.
    plan.in.screenshot = MetaCache::imagePath(key, QStringLiteral("screenshot"));
    plan.in.box        = MetaCache::imagePath(key, QStringLiteral("box"));
    plan.in.logo       = MetaCache::imagePath(key, QStringLiteral("logo"));
    if (plan.in.logo.isEmpty()) plan.in.logo = MetaCache::imagePath(key, QStringLiteral("clearlogo"));
    plan.in.disc       = MetaCache::imagePath(key, QStringLiteral("disc"));
    if (plan.in.disc.isEmpty()) plan.in.disc = MetaCache::imagePath(key, QStringLiteral("cart"));
    if (!hasAnyInput(plan.in)) return plan; // !viable: no card is made — and, by design, no blank one either
    plan.viable = true;

    plan.outPath   = MetaCache::dirFor(key) + QStringLiteral("/miximage.png");
    plan.stampPath = MetaCache::dirFor(key) + QStringLiteral("/miximage.stamp");

    // Staleness is an identity stamp (each input's path + byte size) recorded beside the composite, NOT an
    // mtime comparison. The cache's LRU bumps a served input's mtime once per run, so an mtime check
    // re-composited every item on its first display each run: several image decodes + a PNG encode per row —
    // measured at 150-418ms per nav.select, the themed shelf's scroll lag. A real art change alters a file's
    // size (or a role appears/disappears, changing its path), so the stamp still catches new art; an LRU
    // touch changes neither.
    for (const QString& p : { plan.in.screenshot, plan.in.box, plan.in.logo, plan.in.disc })
        plan.identity += (p.isEmpty() ? QByteArray("-")
                                      : p.toUtf8() + ':' + QByteArray::number(QFileInfo(p).size())) + '\n';
    if (QFileInfo::exists(plan.outPath))
    {
        QFile sf(plan.stampPath);
        plan.fresh = sf.open(QIODevice::ReadOnly) && sf.readAll() == plan.identity;
    }
    return plan;
}

bool Miximage::composeAndSave(const ComposePlan& plan, QSize canvas)
{
    if (!plan.viable || plan.outPath.isEmpty()) return false;
    // Pure image work + QFile writes to plan-named paths only — safe on a worker thread by construction
    // (compose() is documented pure; no MetaCache/QSettings/shared statics are touched here).
    const QImage img = compose(plan.in, canvas);
    if (img.isNull()) return false;
    QDir().mkpath(QFileInfo(plan.outPath).absolutePath());
    if (!img.save(plan.outPath, "PNG")) return false;
    // The stamp write MUST succeed for this to count as done: the async caller re-plans after a success, and
    // a missing/short stamp would read as stale -> recompose -> re-plan -> … an unbounded loop of 100-400ms
    // composes. Failing here makes the caller stop (made=false) and simply retry on a future display.
    QFile sf(plan.stampPath);
    return sf.open(QIODevice::WriteOnly) && sf.write(plan.identity) == plan.identity.size();
}

QString Miximage::ensureForKey(const QString& key, QSize canvas)
{
    const ComposePlan plan = planForKey(key);
    if (!plan.viable) return {};
    if (plan.fresh) return plan.outPath; // inputs unchanged since this composite was made
    if (!composeAndSave(plan, canvas)) return {};
    MetaCache::recordLocalImage(key, QStringLiteral("miximage"), QStringLiteral("miximage.png"));
    return plan.outPath;
}
