// PlayerIcons — the transport glyphs, DRAWN rather than typed.
//
// The transport bar used to be spelled with Unicode media characters (⏮ ⏪ ⏯ ⏩ ⏭ ⏹ 🔊 ⚙). Every one of those
// carries emoji presentation, so Windows resolved them through Segoe UI Emoji and painted them as COLOUR
// bitmaps: a row of saturated blue lozenges that ignored the `color:` in the bar's stylesheet entirely, on a
// surface that is otherwise near-white on near-black. No stylesheet could fix it — a colour font decides its
// own colours — and picking a monochrome family (Segoe UI Symbol has all of them) only moves the problem to
// the platforms that do not ship it, where the fallback is a colour emoji font again.
//
// So the shapes are painted here: triangles, bars, a rounded square, a speaker, a gear. Monochrome by
// construction, tinted by the caller, identical on every platform, and crisp at any device pixel ratio
// because they are geometry rather than a bitmap. This is also what every other player looks like — the icon
// is one flat near-white shape on a dark bar, and colour is reserved for state (focus, hover), not decoration.
//
// Sized in a 24×24 design box and scaled to the requested pixel size, so a single set of coordinates serves
// the 22px bar buttons and any larger surface. Results are cached per (glyph, size, colour, dpr): the bar
// rebuilds its icons on every volume tick, and re-rasterising a gear for each one would be silly.
//
// No Q_OBJECT and no state: free functions, so it needs no moc and can stay header-only.
#pragma once
#include <QAbstractButton>
#include <QColor>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTransform>

namespace PlayerIcons
{

enum Glyph
{
    PrevChapter,   // |◀  jump back to the start of this chapter / the previous one
    Rewind,        // ◀◀  step back a few seconds
    PlayPause,     // ▶‖  one button for both, as the bar has always had it
    Play,
    Pause,
    FastForward,   // ▶▶
    NextChapter,   // ▶|
    Stop,
    Volume,        // speaker + two waves
    VolumeMuted,   // speaker + ×
    VolumeBoost,   // speaker + two waves + a plus (above 100%: software amplification)
    Gear,          // the "more" menu
    Warning,       // the overlay chip that offers another source for a bad stream
};

// The bar's resting ink. Matches the #e8e8e8 the transport's labels already use, so an icon and the time
// readout beside it are the same near-white.
inline QColor defaultColor() { return QColor(0xE8, 0xE8, 0xE8); }

// One size for every transport glyph, everywhere one is drawn. The point of the module is that the play
// button in the video bar and the one in the split pane are the same mark at the same weight, so the size
// lives here rather than at each call site.
inline constexpr int kTransportPx = 22;

namespace Detail
{

// A right-pointing triangle inscribed in `r`, apex at the middle of its right edge.
inline QPainterPath triangle(const QRectF& r)
{
    QPainterPath p;
    p.moveTo(r.left(), r.top());
    p.lineTo(r.right(), r.center().y());
    p.lineTo(r.left(), r.bottom());
    p.closeSubpath();
    return p;
}

inline QPainterPath bar(qreal x, qreal y, qreal w, qreal h)
{
    QPainterPath p;
    p.addRoundedRect(QRectF(x, y, w, h), w / 2.4, w / 2.4);
    return p;
}

// The speaker body: a small rectangle at the left opening out into the cone. Shared by all three volume
// states, which differ only in what is drawn to its right.
inline QPainterPath speaker()
{
    QPainterPath p;
    p.moveTo(3.0, 9.4);
    p.lineTo(6.4, 9.4);
    p.lineTo(11.0, 4.6);
    p.lineTo(11.0, 19.4);
    p.lineTo(6.4, 15.0);
    p.lineTo(3.0, 15.0);
    p.closeSubpath();
    return p;
}

// One of the sound waves off the speaker's mouth: an arc of radius `r` about the cone's axis.
inline QPainterPath wave(qreal r)
{
    QPainterPath p;
    const QRectF box(11.0 - r, 12.0 - r, r * 2, r * 2);
    p.arcMoveTo(box, -52.0);
    p.arcTo(box, -52.0, 104.0);
    return p;
}

// An eight-toothed gear: a disc with the teeth added around it and the hub taken back out, so the whole
// thing is ONE filled path (an outline drawn per part would show seams where the teeth meet the disc).
inline QPainterPath gear()
{
    QPainterPath teeth;
    for (int i = 0; i < 8; ++i)
    {
        QPainterPath t;
        t.addRoundedRect(QRectF(-1.75, -9.6, 3.5, 4.4), 0.9, 0.9);
        teeth.addPath(QTransform().rotate(i * 45.0).map(t));
    }
    QPainterPath disc;
    disc.addEllipse(QPointF(0, 0), 7.0, 7.0);
    QPainterPath hub;
    hub.addEllipse(QPointF(0, 0), 2.9, 2.9);
    return QTransform::fromTranslate(12.0, 12.0).map(disc.united(teeth).subtracted(hub));
}

// The shape of one glyph, in the 24×24 design box.
inline QPainterPath path(Glyph g)
{
    QPainterPath p;
    switch (g)
    {
    case PrevChapter:
        p.addPath(bar(4.2, 5.0, 2.6, 14.0));
        p.addPath(QTransform(-1, 0, 0, 1, 24, 0).map(triangle(QRectF(4.2, 5.0, 11.8, 14.0))));
        break;
    case NextChapter:
        p.addPath(triangle(QRectF(4.2, 5.0, 11.8, 14.0)));
        p.addPath(bar(17.2, 5.0, 2.6, 14.0));
        break;
    case Rewind:
        // Mirrored about the box's centre line, so back and forward are the same shape either way round.
        p.addPath(QTransform(-1, 0, 0, 1, 24, 0).map(triangle(QRectF(3.0, 5.2, 9.2, 13.6))));
        p.addPath(QTransform(-1, 0, 0, 1, 24, 0).map(triangle(QRectF(11.8, 5.2, 9.2, 13.6))));
        break;
    case FastForward:
        p.addPath(triangle(QRectF(3.0, 5.2, 9.2, 13.6)));
        p.addPath(triangle(QRectF(11.8, 5.2, 9.2, 13.6)));
        break;
    case PlayPause:
        // Both halves of what the button does, the way the ⏯ character it replaces read: the triangle first,
        // then the two bars. The button is a toggle with no state feed of its own, so it says "play/pause"
        // rather than claiming to know which one is next.
        p.addPath(triangle(QRectF(3.4, 5.2, 9.4, 13.6)));
        p.addPath(bar(14.6, 5.2, 2.7, 13.6));
        p.addPath(bar(18.7, 5.2, 2.7, 13.6));
        break;
    case Play:
        p.addPath(triangle(QRectF(6.4, 4.6, 12.0, 14.8)));
        break;
    case Pause:
        p.addPath(bar(7.2, 5.0, 3.2, 14.0));
        p.addPath(bar(13.6, 5.0, 3.2, 14.0));
        break;
    case Stop:
        p.addRoundedRect(QRectF(6.6, 6.6, 10.8, 10.8), 1.8, 1.8);
        break;
    case Warning:
    {
        // A rounded triangle with the bar and dot punched out of it, so the mark reads at chip size without
        // needing a second colour to separate the glyph from its plate.
        QPainterPath tri;
        tri.moveTo(12.0, 3.4);
        tri.lineTo(22.2, 20.4);
        tri.lineTo(1.8, 20.4);
        tri.closeSubpath();
        QPainterPath mark;
        mark.addRoundedRect(QRectF(10.9, 9.0, 2.2, 6.2), 1.1, 1.1);
        mark.addEllipse(QPointF(12.0, 17.4), 1.35, 1.35);
        p = tri.subtracted(mark);
        break;
    }
    case Volume:
    case VolumeMuted:
    case VolumeBoost:
    case Gear:
        break; // stroked, not filled — see pixmap()
    }
    return p;
}

} // namespace Detail

// The glyph as a pixmap `px` logical pixels square, in `c`, at `dpr` device pixels per logical pixel.
inline QPixmap pixmap(Glyph g, int px, const QColor& c, qreal dpr)
{
    QPixmap pm(int(px * dpr), int(px * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(px / 24.0, px / 24.0);
    p.setBrush(c);
    p.setPen(Qt::NoPen);

    switch (g)
    {
    case Volume:
    case VolumeMuted:
    case VolumeBoost:
    {
        p.drawPath(Detail::speaker());
        QPen stroke(c);
        stroke.setWidthF(1.9);
        stroke.setCapStyle(Qt::RoundCap);
        p.setPen(stroke);
        p.setBrush(Qt::NoBrush);
        if (g == VolumeMuted)
        {
            // A cross where the waves would be, rather than a slash across the whole icon: at 22px a slash
            // and the cone merge into an unreadable blob.
            p.drawLine(QPointF(15.2, 9.2), QPointF(20.8, 14.8));
            p.drawLine(QPointF(20.8, 9.2), QPointF(15.2, 14.8));
        }
        else
        {
            p.drawPath(Detail::wave(4.6));
            p.drawPath(Detail::wave(7.6));
            if (g == VolumeBoost)
            {
                // Above 100% is amplification, not just "loud" — the plus is what tells the two apart at a
                // glance, and it sits clear of the waves rather than on top of them.
                p.drawLine(QPointF(18.2, 4.6), QPointF(22.2, 4.6));
                p.drawLine(QPointF(20.2, 2.6), QPointF(20.2, 6.6));
            }
        }
        break;
    }
    case Gear:
        p.drawPath(Detail::gear());
        break;
    case Warning:
        // Filled only, with NO softening stroke: this shape's mark is a hole, and a stroke of the icon's own
        // colour would paint that hole half shut.
        p.drawPath(Detail::path(g));
        break;
    default:
    {
        // The filled shapes get a hairline stroke of their own colour so the triangles' points come out
        // softened rather than needle-sharp — the difference between a drawn icon and a clip-art one.
        QPainterPath path = Detail::path(g);
        QPen soften(c);
        soften.setWidthF(0.9);
        soften.setJoinStyle(Qt::RoundJoin);
        soften.setCapStyle(Qt::RoundCap);
        p.setPen(soften);
        p.drawPath(path);
        break;
    }
    }
    p.end();
    return pm;
}

// Cached. The bar rebuilds the volume icon on every slider tick and its whole row on every theme change, so
// the same handful of rasterisations would otherwise be redone dozens of times a second. GUI thread only,
// like every other QPixmap.
inline QIcon icon(Glyph g, int px = 22, const QColor& c = defaultColor(), qreal dpr = 0.0)
{
    if (dpr <= 0.0) dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const quint64 key = (quint64(g) << 48) | (quint64(px & 0xFFF) << 36)
                      | (quint64(quint32(c.rgba())) << 4) | quint64(qBound(1, int(dpr * 2), 15) & 0xF);
    static QHash<quint64, QIcon> cache;
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;
    return *cache.insert(key, QIcon(pixmap(g, px, c, dpr)));
}

// Put `g` on a button IN PLACE OF its text. The text is cleared rather than left beside the icon: these
// buttons used to BE their glyph, so anything left there would print next to the drawn one. Every caller
// that swaps a glyph for a state change (the speaker's three) comes back through here, which is what keeps
// the icon size from drifting between the first paint and the next.
inline void apply(QAbstractButton* b, Glyph g, int px = kTransportPx, const QColor& c = defaultColor())
{
    b->setIcon(icon(g, px, c));
    b->setIconSize(QSize(px, px));
    b->setText(QString());
}

} // namespace PlayerIcons
