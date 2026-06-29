// Headless check of the MOBI reader: parse a .mobi and print its metadata + a snippet of the extracted text.
#include "MobiBook.h"
#include <QCoreApplication>
#include <QFile>
#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: probe_mobi <file.mobi>\n"); return 2; }

    MobiBook b;
    QString err;
    if (!b.open(QString::fromLocal8Bit(argv[1]), &err))
    {
        std::printf("FAIL: %s\n", err.toUtf8().constData());
        return 1;
    }
    std::printf("OK  title=\"%s\"  author=\"%s\"  chapters=%lld\n",
                b.title().toUtf8().constData(), b.author().toUtf8().constData(),
                static_cast<long long>(b.chapterFiles().size()));

    QFile f(b.chapterFiles().first());
    if (f.open(QIODevice::ReadOnly))
    {
        const QByteArray d = f.readAll();
        std::printf("staged HTML: %lld bytes\n--- first 900 chars ---\n%s\n",
                    static_cast<long long>(d.size()), d.left(900).constData());
    }
    return 0;
}
