// Headless check of the reader's flow-anchored pagination: pages flow from a top text offset, so the first
// word must stay put when the window resizes. We lay a long chapter out at one width, page partway in, grab
// the top offset, then re-lay at a different width and confirm the offset still starts the same word. Also
// renders a page to PNG to eyeball margins + footer. Usage: probe_pageview <book> <outdir>
#include "MobiBook.h"
#include <QGuiApplication>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextLine>
#include <QTextCursor>
#include <QFontMetricsF>
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QImage>
#include <QFile>
#include <QVector>
#include <cstdio>

struct Line { qreal y, h; int pos; };

// Mirror BookPageWidget: the text column is capped to ~78 chars, so a wider window doesn't reflow.
static qreal cappedWidth(QTextDocument& doc, int windowW)
{
    const qreal avail = windowW - 2 * 40.0; // 40 = side margin
    const qreal cap = QFontMetricsF(doc.defaultFont()).averageCharWidth() * 78.0;
    return qMax(1.0, qMin(avail, cap));
}

static QVector<Line> layout(QTextDocument& doc, const QString& html, int pt, int windowW)
{
    doc.setDocumentMargin(0);
    doc.setDefaultStyleSheet(QStringLiteral("body{margin:0;} p{margin:0 0 0.7em 0;}"));
    doc.setHtml(html);
    QFont f = doc.defaultFont(); f.setPointSize(pt); doc.setDefaultFont(f);
    doc.setTextWidth(cappedWidth(doc, windowW));

    QVector<Line> ls;
    auto* lay = doc.documentLayout();
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
    {
        QTextLayout* tl = b.layout();
        const qreal top = lay->blockBoundingRect(b).top();
        for (int i = 0; i < tl->lineCount(); ++i)
            ls.push_back({ top + tl->lineAt(i).y(), tl->lineAt(i).height(), b.position() + tl->lineAt(i).textStart() });
    }
    return ls;
}

static int lineForPos(const QVector<Line>& ls, int pos)
{
    int ans = 0;
    for (int i = 0; i < ls.size(); ++i) { if (ls[i].pos <= pos) ans = i; else break; }
    return ans;
}

static int lastFitting(const QVector<Line>& ls, int start, qreal ph)
{
    int m = start;
    while (m + 1 < ls.size() && (ls[m + 1].y + ls[m + 1].h - ls[start].y) <= ph) ++m;
    return m;
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 3) { std::fprintf(stderr, "usage: probe_pageview <book> <outdir>\n"); return 2; }

    MobiBook book; QString err;
    if (!book.open(QString::fromLocal8Bit(argv[1]), &err)) { std::fprintf(stderr, "open: %s\n", err.toLocal8Bit().constData()); return 1; }
    const QStringList files = book.chapterFiles();
    if (files.isEmpty()) { std::fprintf(stderr, "no chapters\n"); return 1; }
    QFile f(files.first()); QString html;
    if (f.open(QIODevice::ReadOnly)) { html = QString::fromUtf8(f.readAll()); f.close(); }

    const qreal SIDE = 40, TOP = 56, BOT = 40;
    const QString out(QString::fromLocal8Bit(argv[2]));

    auto firstWords = [&](QTextDocument& d, int pos) {
        QTextCursor c(&d); c.setPosition(pos);
        c.setPosition(qMin(d.characterCount() - 1, pos + 40), QTextCursor::KeepAnchor);
        return c.selectedText().simplified();
    };

    // --- size A: page partway in, capture the top offset -------------------------------------------------
    const int WA = 800, HA = 600; const qreal phA = HA - TOP - BOT;
    QTextDocument a; QVector<Line> la = layout(a, html, 14, WA);
    int top = la.first().pos;                          // walk forward ~40 pages by whole lines
    for (int n = 0; n < 40; ++n) { int next = lastFitting(la, lineForPos(la, top), phA) + 1; if (next >= la.size()) break; top = la[next].pos; }
    // Force the anchor onto a SOFT-WRAP line (mid-paragraph), the hard case: its start moves when text
    // reflows, unlike a paragraph start.
    for (int i = lineForPos(la, top); i < la.size(); ++i)
        if (a.findBlock(la[i].pos).position() != la[i].pos) { top = la[i].pos; break; }
    const int anchor = top;
    const QString wordsA = firstWords(a, anchor);
    std::printf("(anchor is a %s)\n", a.findBlock(anchor).position() == anchor ? "paragraph start" : "mid-paragraph wrap");

    std::printf("anchor offset %d  size A (800w): \"%s\"\n", anchor, wordsA.toLocal8Bit().constData());

    // --- re-find the line for that offset at many widths ------------------------------------------------
    for (int WB : { 360, 480, 650, 1000, 1200 })
    {
        QTextDocument b; QVector<Line> lb = layout(b, html, 14, WB);
        const int topB = lb[lineForPos(lb, anchor)].pos;   // snap to the line holding the anchor
        const QString wordsB = firstWords(b, topB);
        std::printf("  %4dw: top@%d (drift %+d) \"%s\"  %s\n", WB, topB, topB - anchor,
                    wordsB.toLocal8Bit().constData(), wordsB == wordsA ? "MATCH" : "shift");
    }
    const int WB = 1000, HB = 700; const qreal phB = HB - TOP - BOT;
    QTextDocument b; QVector<Line> lb = layout(b, html, 14, WB);
    const int topB = lb[lineForPos(lb, anchor)].pos;

    // --- render size B's page to PNG (wide window -> centred capped column) ------------------------------
    const qreal cw = cappedWidth(b, WB), cl = qMax(SIDE, (WB - cw) / 2.0);
    const int start = lineForPos(lb, topB), end = lastFitting(lb, start, phB);
    const qreal y0 = lb[start].y, slice = lb[end].y + lb[end].h - y0;
    QImage img(WB, HB, QImage::Format_RGB32); img.fill(Qt::white);
    QPainter p(&img);
    p.fillRect(QRectF(0, 0, WB, TOP), QColor(230, 230, 230)); // menu strip
    p.save();
    p.setClipRect(QRectF(cl, TOP, cw, slice));
    p.translate(cl, TOP - y0);
    QAbstractTextDocumentLayout::PaintContext ctx; ctx.palette.setColor(QPalette::Text, Qt::black);
    ctx.clip = QRectF(0, y0, cw, slice);
    b.documentLayout()->draw(&p, ctx);
    p.restore();
    QColor ink(0, 0, 0, 140); p.setPen(ink);
    p.drawText(QRectF(0, HB - BOT, WB, BOT), Qt::AlignHCenter | Qt::AlignVCenter, QString("resized page"));
    p.end();
    img.save(out + QStringLiteral("/resized.png"));
    std::printf("wrote %s/resized.png\n", out.toLocal8Bit().constData());
    return 0;
}
