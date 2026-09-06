#include "ReadingModes.h"
#include "ComicName.h"

#include <QFileInfo>

#include <algorithm>

namespace ComicRead
{

Mode resolveMode(ComicInfo::Direction embedded, int directionOverride, int modeOverride)
{
    // 1. The mode the user chose for this series wins outright. Anything stored that is not one of the three
    //    values is treated as "never set" — the same forgiving read Settings::comicDirectionOverride makes,
    //    so a hand-edited ini cannot put the reader into a mode that does not exist.
    if (modeOverride == int(Mode::PagedLtr)) return Mode::PagedLtr;
    if (modeOverride == int(Mode::PagedRtl)) return Mode::PagedRtl;
    if (modeOverride == int(Mode::Webtoon))  return Mode::Webtoon;

    // 2 + 3. No mode override: fall through to #152's question, unchanged and through #152's own resolver, so
    // there is still exactly one place that decides what a direction override beats.
    const ComicInfo::Direction over = directionOverride == 1 ? ComicInfo::Direction::LeftToRight
                                    : directionOverride == 2 ? ComicInfo::Direction::RightToLeft
                                                             : ComicInfo::Direction::Unspecified;
    return ComicInfo::resolveDirection(embedded, over) == ComicInfo::Direction::RightToLeft
               ? Mode::PagedRtl : Mode::PagedLtr;
}

bool shouldSplit(Split over, const QSize& page, const QSize& viewport)
{
    if (over == Split::Never) return false;
    if (page.width() <= 0 || page.height() <= 0) return false;   // an undecodable page is never split
    if (over == Split::Always) return true;
    if (page.width() <= page.height()) return false;             // strictly wider than tall; a square is not a spread
    if (viewport.width() <= 0 || viewport.height() <= 0) return true;  // no viewport yet: the page's shape decides
    return viewport.width() <= viewport.height();                // ... and only on a viewport that is not landscape
}

QRect splitHalfRect(const QSize& page, int order, bool rtl)
{
    const int w = qMax(1, page.width()), h = qMax(1, page.height());
    const int leftW = w / 2;
    const QRect left(0, 0, leftW, h);
    const QRect right(leftW, 0, w - leftW, h);   // the odd column stays with the right half; the two tile the page
    // `order` is reading order: in a right-to-left comic the RIGHT half is the one read first.
    const bool wantRight = rtl ? (order <= 0) : (order >= 1);
    return wantRight ? right : left;
}

// ---- Border crop ---------------------------------------------------------------------------------------------

namespace
{
    inline int channelDiff(QRgb a, QRgb b)
    {
        return qMax(qAbs(qRed(a) - qRed(b)),
                    qMax(qAbs(qGreen(a) - qGreen(b)), qAbs(qBlue(a) - qBlue(b))));
    }

    inline bool rowIsBackground(const QImage& img, int y, int x0, int x1, QRgb bg, int tol)
    {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = x0; x <= x1; ++x)
            if (channelDiff(line[x], bg) > tol) return false;
        return true;
    }

    inline bool colIsBackground(const QImage& img, int x, int y0, int y1, QRgb bg, int tol)
    {
        for (int y = y0; y <= y1; ++y)
        {
            const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            if (channelDiff(line[x], bg) > tol) return false;
        }
        return true;
    }
}

QRect cropRect(const QImage& img, int tolerance)
{
    const QRect full(0, 0, img.width(), img.height());
    if (img.width() < kMinCroppedPx || img.height() < kMinCroppedPx) return full;

    const QImage s = img.format() == QImage::Format_RGB32 ? img : img.convertToFormat(QImage::Format_RGB32);
    if (s.isNull()) return full;
    const int w = s.width(), h = s.height();

    // The four corners must agree, or this page has art at an edge and is left alone (see ReadingModes.h).
    const QRgb bg = s.pixel(0, 0);
    if (channelDiff(s.pixel(w - 1, 0), bg) > tolerance) return full;
    if (channelDiff(s.pixel(0, h - 1), bg) > tolerance) return full;
    if (channelDiff(s.pixel(w - 1, h - 1), bg) > tolerance) return full;

    const int maxTrimV = h * kMaxTrimPercent / 100;
    const int maxTrimH = w * kMaxTrimPercent / 100;

    int top = 0, bottom = h - 1;
    while (top < maxTrimV && rowIsBackground(s, top, 0, w - 1, bg, tolerance)) ++top;
    while (h - 1 - bottom < maxTrimV && bottom > top && rowIsBackground(s, bottom, 0, w - 1, bg, tolerance)) --bottom;

    // Columns are scanned only over the rows that survived, so an L-shaped margin goes too.
    int left = 0, right = w - 1;
    while (left < maxTrimH && colIsBackground(s, left, top, bottom, bg, tolerance)) ++left;
    while (w - 1 - right < maxTrimH && right > left && colIsBackground(s, right, top, bottom, bg, tolerance)) --right;

    const QRect r(left, top, right - left + 1, bottom - top + 1);
    if (r.width() < kMinCroppedPx || r.height() < kMinCroppedPx) return full;
    if (r.width() * 100 < w * kMinKeepPercent || r.height() * 100 < h * kMinKeepPercent) return full;
    return r;
}

// ---- Colour ------------------------------------------------------------------------------------------------

ColorAdjust adjustFor(Filter f)
{
    ColorAdjust a;
    switch (f)
    {
    case Filter::None:         break;
    case Filter::Greyscale:    a.greyscale = true; break;
    case Filter::Sepia:        a.greyscale = true; a.warmth = 60; break;
    case Filter::Night:        a.brightness = -35; a.warmth = 25; break;
    case Filter::HighContrast: a.contrast = 40; break;
    }
    return a;
}

QRgb adjustPixel(QRgb p, const ColorAdjust& a)
{
    int r = qRed(p), g = qGreen(p), b = qBlue(p);
    if (a.greyscale) { const int y = qGray(p); r = g = b = y; }
    if (a.brightness != 0)
    {
        const int d = a.brightness * 255 / 100;
        r += d; g += d; b += d;
    }
    if (a.contrast != 0)
    {
        const int num = 100 + a.contrast;
        r = 128 + (r - 128) * num / 100;
        g = 128 + (g - 128) * num / 100;
        b = 128 + (b - 128) * num / 100;
    }
    if (a.warmth != 0)
    {
        const int d = a.warmth * 40 / 100;
        r += d; b -= d;
    }
    const auto clamp8 = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    return qRgba(clamp8(r), clamp8(g), clamp8(b), qAlpha(p));
}

QImage applyAdjust(const QImage& src, const ColorAdjust& a)
{
    if (a.isIdentity() || src.isNull()) return src;
    QImage out = src.convertToFormat(src.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    if (out.isNull()) return src;
    const int w = out.width(), h = out.height();
    for (int y = 0; y < h; ++y)
    {
        QRgb* line = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) line[x] = adjustPixel(line[x], a);
    }
    return out;
}

QImage preparePage(const QImage& src, const PageOptions& o)
{
    if (src.isNull()) return src;
    QImage img = src;
    if (o.crop)
    {
        const QRect r = cropRect(img, o.cropTolerance);
        if (r != QRect(0, 0, img.width(), img.height())) img = img.copy(r);
    }
    if (o.half >= 0) img = img.copy(splitHalfRect(img.size(), o.half, o.rtl));
    return applyAdjust(img, o.adjust);
}

// ---- Webtoon -------------------------------------------------------------------------------------------------

Strip stripLayout(const QVector<QSize>& pageSizes, int viewportW)
{
    Strip s;
    s.width = qMax(1, viewportW);
    s.tops.reserve(pageSizes.size());
    s.heights.reserve(pageSizes.size());
    int y = 0;
    for (const QSize& sz : pageSizes)
    {
        const int drawn = (sz.width() > 0 && sz.height() > 0)
                              ? qMax(1, qRound(double(sz.height()) * double(s.width) / double(sz.width())))
                              : qMax(1, int(s.width * kUnreadablePageAspect));
        s.tops.append(y);
        s.heights.append(drawn);
        y += drawn;
    }
    s.totalHeight = y;
    return s;
}

int stripOffset(const Strip& s, int page, double fraction)
{
    if (s.tops.isEmpty()) return 0;
    const int p = qBound(0, page, s.tops.size() - 1);
    const double f = qBound(0.0, fraction, 1.0);
    // qRound, not a truncation: this is the inverse of stripPositionAt's division, and truncating it would
    // lose the resume position by a pixel every time the double came back a hair under the integer.
    return s.tops[p] + qRound(f * double(s.heights[p]));
}

void stripPositionAt(const Strip& s, int y, int* page, double* fraction)
{
    if (page) *page = 0;
    if (fraction) *fraction = 0.0;
    if (s.tops.isEmpty()) return;
    const int clamped = qBound(0, y, qMax(0, s.totalHeight - 1));
    // The last page whose top is at or above `y` — std::upper_bound lands one past it.
    const auto it = std::upper_bound(s.tops.cbegin(), s.tops.cend(), clamped);
    const int p = qBound(0, int(it - s.tops.cbegin()) - 1, s.tops.size() - 1);
    if (page) *page = p;
    if (fraction) *fraction = double(clamped - s.tops[p]) / double(qMax(1, s.heights[p]));
}

QVector<int> prefetchWindow(int current, int total, int radius)
{
    QVector<int> out;
    if (total <= 0) return out;
    const int c = qBound(0, current, total - 1);
    out.append(c);
    for (int d = 1; d <= qMax(0, radius); ++d)
    {
        if (c + d < total) out.append(c + d);   // forward first: it is where the reader is going
        if (c - d >= 0)    out.append(c - d);
    }
    return out;
}

// ---- The store key -------------------------------------------------------------------------------------------

QString seriesKeyFor(const QString& comicInfoSeries, const QString& filePath)
{
    if (!comicInfoSeries.isEmpty()) return ComicName::seriesKey(comicInfoSeries);
    if (filePath.isEmpty()) return QString();
    const ComicName::Parsed p = ComicName::parse(QFileInfo(filePath).completeBaseName());
    if (p.evidence != ComicName::Evidence::None && !p.series.isEmpty())
        return ComicName::seriesKey(p.series);
    return ComicName::seriesKey(p.cleaned);   // cleaned is never empty (ComicName.h)
}

} // namespace ComicRead
