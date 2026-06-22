#include "EpubBook.h"
#include <QCoreApplication>
#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) { printf("usage: probe_epub <file.epub>\n"); return 2; }

    EpubBook b;
    QString err;
    if (!b.open(QString::fromLocal8Bit(argv[1]), &err))
    { printf("open failed: %s\n", err.toUtf8().constData()); return 1; }

    printf("title:    %s\n", b.title().toUtf8().constData());
    printf("author:   %s\n", b.author().toUtf8().constData());
    printf("chapters: %d\n", int(b.chapterFiles().size()));
    printf("toc:      %d\n", int(b.toc().size()));
    int n = 0;
    for (const EpubTocEntry& e : b.toc())
    {
        if (n++ >= 10) { printf("  ... (%d more)\n", int(b.toc().size()) - 10); break; }
        const int ci = b.chapterIndexForHref(e.href);
        printf("  toc: \"%s\" -> %s  (chapter %d)\n", e.title.toUtf8().constData(), e.href.toUtf8().constData(), ci);
    }
    for (int i = 0; i < b.chapterFiles().size() && i < 3; ++i)
        printf("  ch[%d]: %s\n", i, b.chapterFiles()[i].toUtf8().constData());
    return 0;
}
