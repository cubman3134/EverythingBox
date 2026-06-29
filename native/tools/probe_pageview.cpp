// Headless check of the paginated reader's core: load a long chapter into a QTextDocument paginated by
// page size (as BookPageWidget does) and render a few pages to PNG so we can eyeball that text is drawn,
// paginated, and never cut at the page boundary. Usage: probe_pageview <book> <outdir>
#include "MobiBook.h"
#include <QGuiApplication>
#include <QTextDocument>
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
    const qreal M = 40.0;
    QTextDocument doc;
    doc.setDocumentMargin(0);
    doc.setDefaultStyleSheet(QStringLiteral("body{margin:0;} p{margin:0 0 0.7em 0;}"));
    doc.setHtml(html);
    QFont font = doc.defaultFont(); font.setPointSize(14); doc.setDefaultFont(font);
    doc.setPageSize(QSizeF(W - 2 * M, H - 2 * M));
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
        p.setClipRect(QRectF(M, M, pageW, pageH));
        p.translate(M, M);
        p.translate(0, -pg * pageH);
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, Qt::black);
        ctx.clip = QRectF(0, pg * pageH, pageW, pageH);
        doc.documentLayout()->draw(&p, ctx);
        p.end();
        const QString out = outdir + QStringLiteral("/page-%1.png").arg(pg);
        img.save(out);
        std::printf("wrote %s\n", out.toLocal8Bit().constData());
    }
    return 0;
}
