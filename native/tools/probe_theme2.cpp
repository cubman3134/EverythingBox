// Phase-1 verification of the QML theme engine: parse a theme file, inject it (plus a sample catalog) into
// the QML view, render one frame headlessly, and save a PNG - proving the theme-file -> QML -> pixels
// pipeline end to end. Usage: probe_theme2 <theme.json> <view.qml> <out.png>
#include <QGuiApplication>
#include <QQuickView>
#include <QQmlContext>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QImage>
#include <QUrl>
#include <QVariant>
#include <cstdio>

int main(int argc, char** argv)
{
    qputenv("QT_QUICK_BACKEND", "software"); // software scene graph -> renders without a GPU
    QGuiApplication app(argc, argv);
    if (argc < 4) { std::fprintf(stderr, "usage: probe_theme2 <theme.json> <view.qml> <out.png>\n"); return 2; }

    QFile tf(QString::fromLocal8Bit(argv[1]));
    QVariantMap theme;
    if (tf.open(QIODevice::ReadOnly))
        theme = QJsonDocument::fromJson(tf.readAll()).object().toVariantMap();
    else { std::fprintf(stderr, "can't read theme %s\n", argv[1]); return 1; }

    // A stand-in catalog (Phase 3 will feed the app's real rows here).
    const char* names[] = { "Movies", "TV", "Music", "Books", "Comics", "Live TV", "Live Sports", "Games", "Podcasts", "Audiobooks", "Manga", "Retro" };
    const char* cols[]  = { "#C0392B", "#2980B9", "#8E44AD", "#3E8E7E", "#E07A2E", "#E23B3B", "#2EA043", "#7A5BD0", "#C44ED1", "#5B7FE0", "#CE5797", "#16A085" };
    QVariantList items;
    for (int i = 0; i < 12; ++i) { QVariantMap m; m["title"] = names[i]; m["accent"] = cols[i]; items << m; }

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(1280, 720);
    view.rootContext()->setContextProperty("theme", theme);
    view.rootContext()->setContextProperty("items", items);
    view.setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[2])));

    if (view.status() != QQuickView::Ready)
    {
        for (const QQmlError& e : view.errors())
            std::fprintf(stderr, "QML: %s\n", e.toString().toUtf8().constData());
        return 1;
    }

    view.show();
    QImage img = view.grabWindow();
    if (img.isNull()) { std::fprintf(stderr, "grabWindow returned null (no render surface)\n"); return 1; }
    if (!img.save(QString::fromLocal8Bit(argv[3]))) { std::fprintf(stderr, "save failed\n"); return 1; }
    std::printf("OK: theme \"%s\" rendered %dx%d -> %s\n",
                theme.value("name").toString().toUtf8().constData(), img.width(), img.height(), argv[3]);
    return 0;
}
