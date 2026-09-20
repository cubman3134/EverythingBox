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

// ---- Scan-quality corrections (increment 2) ------------------------------------------------------------------
//
// All three are 3x3 neighbourhood passes with REPLICATE padding: a sample outside the image is the nearest
// edge pixel. That one convention is what makes each of them total - a 1x1 page is its own neighbourhood, so
// there is no smallest-size special case to get wrong - and it is the convention the probe's border oracles
// are computed against. See ReadingModes.h for the kernels and for why they compose in the order they do.

namespace
{
    // 32-bit, so a scan line is an array of QRgb and every read below is one load. Alpha is carried from the
    // CENTRE pixel and never filtered: a comic page is opaque, and averaging alpha would only matter for the
    // one page that is not, where it would be wrong in a way nobody could see coming.
    inline QImage to32(const QImage& s)
    {
        const QImage::Format f = s.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32;
        return s.format() == f ? s : s.convertToFormat(f);
    }

    inline int clampByte(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

    // The [1 2 1] HORIZONTAL half of the binomial kernel over one row, replicate padded, written into `out`
    // as three ints per pixel. UNDIVIDED on purpose: the vertical half adds these and divides by 16 once, so
    // the integer answer is the exact 2D kernel's rather than two roundings of it. Each entry is at most
    // 255 * 4 = 1020.
    void rowSums121(const QImage& in, int y, int* out)
    {
        const int w = in.width();
        const QRgb* line = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        for (int x = 0; x < w; ++x)
        {
            const QRgb a = line[x > 0 ? x - 1 : 0];
            const QRgb b = line[x];
            const QRgb c = line[x + 1 < w ? x + 1 : w - 1];
            int* o = out + x * 3;
            o[0] = qRed(a)   + 2 * qRed(b)   + qRed(c);
            o[1] = qGreen(a) + 2 * qGreen(b) + qGreen(c);
            o[2] = qBlue(a)  + 2 * qBlue(b)  + qBlue(c);
        }
    }

    // The binomial low-pass itself, as ONE routine both de-moire and sharpen go through: de-moire returns
    // `blur` and sharpen subtracts it from the original. Only THREE rows of horizontal sums are ever held
    // (the row above, the row itself, the row below), so a 12000-pixel-tall webtoon page costs three row
    // buffers and not a second image's worth of intermediates.
    QImage binomialBlur(const QImage& in)
    {
        const int w = in.width(), h = in.height();
        QImage out(w, h, in.format());
        if (out.isNull()) return QImage();
        QVector<int> buf(3 * w * 3);
        int* a = buf.data();                 // the row ABOVE (y-1, clamped)
        int* b = buf.data() + w * 3;         // the row itself
        int* c = buf.data() + w * 3 * 2;     // the row BELOW (y+1, clamped)
        rowSums121(in, 0, b);
        std::copy(b, b + w * 3, a);          // y = -1 replicates row 0
        rowSums121(in, h > 1 ? 1 : 0, c);
        for (int y = 0; y < h; ++y)
        {
            const QRgb* srcLine = reinterpret_cast<const QRgb*>(in.constScanLine(y));
            QRgb* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < w; ++x)
            {
                const int i = x * 3;
                // (a + 2b + c + 8) / 16 - the +8 is the round-to-nearest, and the maximum is
                // (1020 + 2040 + 1020 + 8) / 16 = 255, so no clamp is possible here.
                dst[x] = qRgba((a[i]     + 2 * b[i]     + c[i]     + 8) / 16,
                               (a[i + 1] + 2 * b[i + 1] + c[i + 1] + 8) / 16,
                               (a[i + 2] + 2 * b[i + 2] + c[i + 2] + 8) / 16,
                               qAlpha(srcLine[x]));
            }
            int* t = a; a = b; b = c; c = t;             // shift the window down one row...
            rowSums121(in, qMin(y + 2, h - 1), c);       // ... and fill the new bottom (clamped at the end)
        }
        return out;
    }

    // The median of five, in six compare-exchanges. min(a,b,d,e) is below at least three of the five and
    // max(a,b,d,e) is above at least three, so neither can be the third-smallest: both are discarded and the
    // median of the five is the median of what is left.
    inline int median5(int a, int b, int c, int d, int e)
    {
        const int p = qMin(a, b), q = qMax(a, b);
        const int r = qMin(d, e), s = qMax(d, e);
        const int lo = qMax(p, r);                       // drop min(a,b,d,e)
        const int hi = qMin(q, s);                       // drop max(a,b,d,e)
        return qMax(qMin(lo, c), qMin(qMax(lo, c), hi)); // median of three
    }
}

QImage demoire(const QImage& src)
{
    if (src.isNull()) return src;
    const QImage in = to32(src);
    if (in.isNull()) return src;
    const QImage out = binomialBlur(in);
    return out.isNull() ? src : out;
}

QImage denoise(const QImage& src)
{
    if (src.isNull()) return src;
    const QImage in = to32(src);
    if (in.isNull()) return src;
    const int w = in.width(), h = in.height();
    QImage out(w, h, in.format());
    if (out.isNull()) return src;
    for (int y = 0; y < h; ++y)
    {
        const QRgb* up  = reinterpret_cast<const QRgb*>(in.constScanLine(y > 0 ? y - 1 : 0));
        const QRgb* mid = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        const QRgb* dn  = reinterpret_cast<const QRgb*>(in.constScanLine(y + 1 < h ? y + 1 : h - 1));
        QRgb* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x)
        {
            const QRgb l = mid[x > 0 ? x - 1 : 0];
            const QRgb m = mid[x];
            const QRgb r = mid[x + 1 < w ? x + 1 : w - 1];
            const QRgb u = up[x], d = dn[x];
            dst[x] = qRgba(median5(qRed(l),   qRed(m),   qRed(r),   qRed(u),   qRed(d)),
                           median5(qGreen(l), qGreen(m), qGreen(r), qGreen(u), qGreen(d)),
                           median5(qBlue(l),  qBlue(m),  qBlue(r),  qBlue(u),  qBlue(d)),
                           qAlpha(m));
        }
    }
    return out;
}

QImage sharpen(const QImage& src)
{
    if (src.isNull()) return src;
    const QImage in = to32(src);
    if (in.isNull()) return src;
    const QImage blur = binomialBlur(in);
    if (blur.isNull()) return src;
    const int w = in.width(), h = in.height();
    QImage out(w, h, in.format());
    if (out.isNull()) return src;
    // centre + (centre - blur) * kSharpenPercent/100, written as one rounded expression per channel. At the
    // shipped 50% that is (3*c - blur + 1) / 2; the guard keeps the division off a negative numerator, where
    // C++ would truncate towards zero instead of down - and where the answer clamps to 0 regardless.
    const auto mask = [](int c, int b) {
        const int v = (100 + kSharpenPercent) * c - kSharpenPercent * b + 50;
        return v <= 0 ? 0 : clampByte(v / 100);
    };
    for (int y = 0; y < h; ++y)
    {
        const QRgb* s = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        const QRgb* b = reinterpret_cast<const QRgb*>(blur.constScanLine(y));
        QRgb* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x)
            dst[x] = qRgba(mask(qRed(s[x]),   qRed(b[x])),
                           mask(qGreen(s[x]), qGreen(b[x])),
                           mask(qBlue(s[x]),  qBlue(b[x])),
                           qAlpha(s[x]));
    }
    return out;
}

QImage applyScanFixes(const QImage& src, const ScanFixes& f)
{
    if (f.isIdentity() || src.isNull()) return src;   // "off" is the image itself, not a copy of it
    QImage img = src;
    if (f.demoire) img = demoire(img);
    if (f.denoise) img = denoise(img);
    if (f.sharpen) img = sharpen(img);
    return img;
}

// The ladder the one control cycles. Written out rather than computed from the three bits, because the order
// is a reading decision (the single corrections first, the combination a scanned print needs last) and not a
// binary count - a count would put "de-moire + denoise" between "denoise" and "sharpen".
ScanFixes scanPreset(int index)
{
    ScanFixes f;
    switch (index)
    {
    case 1: f.demoire = true; break;
    case 2: f.denoise = true; break;
    case 3: f.sharpen = true; break;
    case 4: f.demoire = f.denoise = f.sharpen = true; break;
    default: break;   // 0, and anything off the ladder, is off
    }
    return f;
}

int scanPresetIndex(const ScanFixes& f)
{
    for (int i = 0; i < kScanPresetCount; ++i)
        if (scanPreset(i) == f) return i;
    return -1;   // a combination the ladder does not name (only a hand-edited store can produce one)
}

ScanFixes nextScanPreset(const ScanFixes& f)
{
    const int i = scanPresetIndex(f);
    return scanPreset(i < 0 ? 0 : (i + 1) % kScanPresetCount);
}

QPoint zoomStartOffset(const QSize& content, const QSize& viewport, ZoomStart z, bool rtl,
                       const QPoint& current)
{
    // The scrollable range, per axis. A page no bigger than the viewport has a range of 0, so every option
    // below collapses to (0, 0) there - which is the only honest answer when there is nowhere to scroll.
    const int maxX = qMax(0, content.width()  - qMax(0, viewport.width()));
    const int maxY = qMax(0, content.height() - qMax(0, viewport.height()));
    switch (z)
    {
    case ZoomStart::Centre:      return QPoint(maxX / 2, maxY / 2);
    case ZoomStart::ReadingSide: return QPoint(rtl ? maxX : 0, 0);
    case ZoomStart::Top:         break;
    }
    // Top: the top of the page, with the horizontal position left exactly where the reader had it - the
    // reader's behaviour before this option existed, clamped into the range this page actually has.
    return QPoint(qBound(0, current.x(), maxX), 0);
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
    // The scan fixes run over the pixels that will actually be shown - after the crop and the split, before
    // the tint (ReadingModes.h states the order and why each step is where it is).
    img = applyScanFixes(img, o.scan);
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

// ---- Webtoon: decoding off the paint path (#286) -------------------------------------------------------------

QVector<int> stripRequests(const QVector<int>& window, const QSet<int>& cached,
                           const QHash<int, quint64>& inFlight, quint64 generation)
{
    QVector<int> out;
    out.reserve(window.size());
    for (int page : window)
    {
        if (cached.contains(page)) continue;
        const auto it = inFlight.constFind(page);
        if (it != inFlight.constEnd() && it.value() == generation) continue;   // an older request does not count
        out.append(page);
    }
    return out;
}

bool acceptStripResult(const StripResult& r, quint64 generation, int width, const QVector<int>& window)
{
    if (r.generation != generation) return false;   // the cache it was meant for has been emptied since
    if (r.width != width) return false;             // scaled for a strip that is no longer this wide
    return window.contains(r.page);                 // the reader has moved on; caching it would leak
}

bool stripJobWanted(int page, quint64 jobGeneration, quint64 liveGeneration, int windowFirst, int windowLast)
{
    return jobGeneration == liveGeneration && page >= windowFirst && page <= windowLast;
}

int stripLastVisible(const Strip& s, int y, int viewportH)
{
    if (s.count() == 0) return 0;
    int p = 0;
    // The last pixel row on screen is y + viewportH - 1; stripPositionAt clamps it to the strip's end.
    stripPositionAt(s, y + qMax(1, viewportH) - 1, &p, nullptr);
    return p;
}

QVector<int> stripWindow(const Strip& s, int current, int lastVisible)
{
    return prefetchWindow(current, s.count(), qMax(kPrefetchRadius, lastVisible - current));
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

bool ownsPointerAt(const QPoint& p, const PointerControls& c)
{
    return (c.railShown && c.rail.contains(p))
        || (c.vBarShown && c.vBar.contains(p))
        || (c.hBarShown && c.hBar.contains(p));
}

} // namespace ComicRead
