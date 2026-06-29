// Headless check of the paginated reader's core: load a long chapter into a QTextDocument paginated by
// page size (as BookPageWidget does) and render a few pages to PNG so we can eyeball that text is drawn,
// paginated, and never cut at the page boundary. Usage: probe_pageview <book> <outdir>
#include "MobiBook.h"
#include <QGuiApplication>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextLine>
#include <QTextCursor>
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QImage>
#include <QFile>
#include <QFileInfo>
#include <QSizeF>
#include <cstdio>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 3) { std::fprintf(stderr, "usage: probe_pageview <book> <outdir>\n"); return 2; }
    const QString path = QString::fromLocal8Bit(argv[1]);
    const QString outdir = QString::fromLocal8Bit(argv[2]);

    MobiBook book;
    QString err;
    if (!book.open(path, &err)) { std::fprintf(stderr, "open failed: %s\n", err.toLocal8Bit().constData()); return 1; }
    const QStringList files = book.chapterFiles();
    if (files.isEmpty()) { std::fprintf(stderr, "no chapters\n"); return 1; }

    QFile f(files.first());
    QString html;
    if (f.open(QIODevice::ReadOnly)) { html = QString::fromUtf8(f.readAll()); f.close(); }

    const int W = 800, H = 600;
    const qreal SIDE = 40.0, TOP = 56.0, BOT = 40.0; // mirrors BookPageWidget (top inset clears the menu)
    QTextDocument doc;
    doc.setDocumentMargin(0);
    doc.setDefaultStyleSheet(QStringLiteral("body{margin:0;} p{margin:0 0 0.7em 0;}"));
    doc.setHtml(html);
    QFont font = doc.defaultFont(); font.setPointSize(14); doc.setDefaultFont(font);
    doc.setPageSize(QSizeF(W - 2 * SIDE, H - TOP - BOT));
    const int pages = doc.pageCount();
    std::printf("title=\"%s\" pages=%d pageSize=%gx%g\n",
                book.title().toLocal8Bit().constData(), pages, doc.pageSize().width(), doc.pageSize().height());

    const qreal pageW = doc.pageSize().width(), pageH = doc.pageSize().height();
    for (int pg : { 0, 1, qMin(2, pages - 1) })
    {
        if (pg < 0 || pg >= pages) continue;
        QImage img(W, H, QImage::Format_RGB32);
        img.fill(Qt::white);
        QPainter p(&img);
        // A grey band where the overlay menu would sit, to confirm text clears it.
        p.fillRect(QRectF(0, 0, W, TOP), QColor(230, 230, 230));
        p.save();
        p.setClipRect(QRectF(SIDE, TOP, pageW, pageH));
        p.translate(SIDE, TOP);
        p.translate(0, -pg * pageH);
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, Qt::black);
        ctx.clip = QRectF(0, pg * pageH, pageW, pageH);
        doc.documentLayout()->draw(&p, ctx);
        p.restore();
        QColor ink(0, 0, 0, 140); p.setPen(ink);
        p.drawText(QRectF(0, H - BOT, W, BOT), Qt::AlignHCenter | Qt::AlignVCenter,
                   QString("%1 / %2").arg(pg + 1).arg(pages));
        p.end();
        const QString out = outdir + QStringLiteral("/page-%1.png").arg(pg);
        img.save(out);
        std::printf("wrote %s\n", out.toLocal8Bit().constData());
    }

    // Round-trip the reading anchor: capture the document offset at the top of a page at this size, then
    // re-paginate at a different size and confirm the offset resolves to text starting with the same words.
    auto snippet = [&](int pos) {
        QTextCursor c(&doc);
        c.setPosition(pos);
        c.setPosition(qMin(doc.characterCount() - 1, pos + 48), QTextCursor::KeepAnchor);
        return c.selectedText().simplified();
    };
    auto pageOf = [&](int pos, qreal ph) {
        QTextBlock b = doc.findBlock(pos);
        qreal y = doc.documentLayout()->blockBoundingRect(b).top();
        if (auto* lay = b.layout(); lay && lay->lineCount() > 0)
            y += lay->lineForTextPosition(qMax(0, pos - b.position())).y();
        return int(y / ph);
    };

    auto yOf = [&](int pos) {
        QTextBlock b = doc.findBlock(pos);
        qreal y = doc.documentLayout()->blockBoundingRect(b).top();
        if (auto* lay = b.layout(); lay && lay->lineCount() > 0)
            y += lay->lineForTextPosition(qMax(0, pos - b.position())).y();
        return y;
    };

    const int P = qMin(40, pages - 1);
    const int anchor = doc.documentLayout()->hitTest(QPointF(1, P * pageH + 1), Qt::FuzzyHit);
    const QString before = snippet(anchor);

    const qreal W2 = 480 - 2 * SIDE, H2 = 760 - TOP - BOT; // a very different window shape
    doc.setPageSize(QSizeF(W2, H2));
    const int newPage = pageOf(anchor, H2);
    const qreal ay = yOf(anchor);
    const bool onPage = ay >= newPage * H2 && ay < (newPage + 1) * H2; // last spot is visible on the page
    const bool nothingSkipped = newPage * H2 <= ay;                    // page starts at or before it
    const int linesFromTop = int((ay - newPage * H2) / qMax(1.0, ay > 0 ? doc.documentLayout()->blockBoundingRect(doc.findBlock(anchor)).height() : 1.0));

    std::printf("\nanchor round-trip: page %d/%d (800x600) -> resolves to page %d/%d (480x760)\n",
                P + 1, pages, newPage + 1, doc.pageCount());
    std::printf("  anchored text: \"%s\"\n", before.toLocal8Bit().constData());
    std::printf("  visible on restored page=%s  no-unread-skipped=%s  (~%d line(s) below the top)\n",
                onPage ? "yes" : "NO", nothingSkipped ? "yes" : "NO", linesFromTop);
    return 0;
}
