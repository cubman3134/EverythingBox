// Round-trip check of the QtPdf renderer: write a 2-page PDF, load it back with QPdfDocument, render a
// page to an image, and confirm it has real (non-white) content. Verifies QtPdf/PDFium works headlessly.
#include <QGuiApplication>
#include <QPdfWriter>
#include <QPdfDocument>
#include <QPainter>
#include <QImage>
#include <QDir>
#include <cstdio>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QString path = QDir::tempPath() + QStringLiteral("/goliath_probe.pdf");

    {
        QPdfWriter w(path);
        w.setPageSize(QPageSize(QPageSize::A4));
        QPainter p(&w);
        QFont f; f.setPointSize(48); p.setFont(f);
        p.drawText(QRect(200, 200, 4000, 1000), Qt::AlignLeft, QStringLiteral("Goliath PDF page 1"));
        w.newPage();
        p.drawText(QRect(200, 200, 4000, 1000), Qt::AlignLeft, QStringLiteral("Goliath PDF page 2"));
        p.end();
    }

    QPdfDocument doc;
    doc.load(path);
    if (doc.status() != QPdfDocument::Status::Ready) { printf("load failed (status=%d)\n", int(doc.status())); return 1; }
    printf("loaded: pageCount=%d\n", doc.pageCount());

    const QImage img = doc.render(0, QSize(850, 1100));
    if (img.isNull()) { printf("render returned a null image\n"); return 1; }

    long nonWhite = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qGray(img.pixel(x, y)) < 200) ++nonWhite;
    printf("rendered page 0: %dx%d, non-white pixels=%ld\n", img.width(), img.height(), nonWhite);
    printf("%s\n", (doc.pageCount() == 2 && nonWhite > 100) ? "PDF RENDERS: QtPdf/PDFium works"
                                                            : "unexpected output");
    QFile::remove(path);
    return (doc.pageCount() == 2 && nonWhite > 100) ? 0 : 1;
}
