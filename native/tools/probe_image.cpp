#include <QGuiApplication>
#include <QPixmap>
#include <cstdio>
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc < 2) { printf("usage: probe_image <file>\n"); return 2; }
    QPixmap pm(QString::fromLocal8Bit(argv[1]));
    if (pm.isNull()) { printf("FAILED to load %s\n", argv[1]); return 1; }
    printf("loaded %s -> %dx%d  (SVG renders to QPixmap)\n", argv[1], pm.width(), pm.height());
    return 0;
}
