// Headless check of the PDF->reflow-text reader: extract a PDF as a book and print metadata + a text snippet.
#include "PdfTextBook.h"
#include <QGuiApplication>
#include <QFile>
#include <cstdio>
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: probe_pdftext <file.pdf>\n"); return 2; }
    PdfTextBook b; QString err;
    if (!b.open(QString::fromLocal8Bit(argv[1]), &err)) { std::printf("FAIL: %s\n", err.toUtf8().constData()); return 1; }
    std::printf("OK  title=\"%s\"  author=\"%s\"  pages(chapters)=%lld\n",
                b.title().toUtf8().constData(), b.author().toUtf8().constData(),
                static_cast<long long>(b.chapterFiles().size()));
    QFile f(b.chapterFiles().first());
    if (f.open(QIODevice::ReadOnly)) { const QByteArray d = f.readAll(); std::printf("--- page 1 (first 700) ---\n%s\n", d.left(700).constData()); }
    return 0;
}
