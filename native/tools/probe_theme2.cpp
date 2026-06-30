// Verification harness for the QML theme engine: parse a theme file, hand its `home` view + a sample catalog
// to the generic ThemeView renderer, render one frame headlessly, and save a PNG. Proves the whole pipeline
// (theme file -> elements -> bindings -> pixels).
// Usage: probe_theme2 <theme.json> <ThemeView.qml> <out.png> [sampleImage]
#include <QGuiApplication>
#include <QQuickView>
#include <QQuickItem>
#include <QQmlContext>
#include <QEventLoop>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QUrl>
#include <QVariant>
#include <cstdio>

int main(int argc, char** argv)
{
    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
    if (argc < 4) { std::fprintf(stderr, "usage: probe_theme2 <theme.json> <ThemeView.qml> <out.png> [sampleImage]\n"); return 2; }
    const QString themePath = QString::fromLocal8Bit(argv[1]);
    const QString sampleImg = argc >= 5 ? QString::fromLocal8Bit(argv[4]) : QString();

    QFile tf(themePath);
    if (!tf.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "can't read %s\n", argv[1]); return 1; }
    const QJsonObject theme = QJsonDocument::fromJson(tf.readAll()).object();
    const QVariantMap view = theme.value("views").toObject().value("home").toObject().toVariantMap();
    if (view.isEmpty()) { std::fprintf(stderr, "theme has no views.home\n"); return 1; }

    // Sample catalog. The first few carry a poster so the bound image/video elements show something real.
    const char* names[] = { "Movies", "TV", "Music", "Books", "Comics", "Live TV", "Live Sports", "Games", "Podcasts", "Audiobooks", "Manga", "Retro" };
    const char* cols[]  = { "#C0392B", "#2980B9", "#8E44AD", "#3E8E7E", "#E07A2E", "#E23B3B", "#2EA043", "#7A5BD0", "#C44ED1", "#5B7FE0", "#CE5797", "#16A085" };
    QVariantList items;
    for (int i = 0; i < 12; ++i)
    {
        QVariantMap m;
        m["title"] = names[i];
        m["accent"] = cols[i];
        m["subtitle"] = QStringLiteral("%1 items").arg((i + 3) * 17);
        m["rating"] = (i % 5) / 5.0 + 0.2;        // 0.2 .. 1.0
        if (i < 3 && !sampleImg.isEmpty()) m["image"] = sampleImg;
        items << m;
    }

    QQuickView qv;
    qv.setResizeMode(QQuickView::SizeRootObjectToView);
    qv.resize(1280, 720);
    qv.rootContext()->setContextProperty("theme", theme.toVariantMap()); // not used by ThemeView, kept for compat
    qv.setInitialProperties({
        { "view", view },
        { "items", items },
        { "system", QVariantMap{ { "name", theme.value("name").toString() } } },
        { "currentIndex", 0 },
        { "base", QUrl::fromLocalFile(QFileInfo(themePath).absolutePath()).toString() }
    });
    qv.setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[2])));

    if (qv.status() != QQuickView::Ready)
    {
        for (const QQmlError& e : qv.errors())
            std::fprintf(stderr, "QML: %s\n", e.toString().toUtf8().constData());
        return 1;
    }

    qv.show();
    // Let the Repeater/Loaders incubate and a render pass happen before grabbing.
    { QEventLoop loop; QTimer::singleShot(400, &loop, &QEventLoop::quit); loop.exec(); }
    QImage img = qv.grabWindow();
    if (img.isNull()) { std::fprintf(stderr, "grabWindow returned null\n"); return 1; }
    if (!img.save(QString::fromLocal8Bit(argv[3]))) { std::fprintf(stderr, "save failed\n"); return 1; }
    std::printf("OK: theme \"%s\" rendered %dx%d -> %s\n",
                theme.value("name").toString().toUtf8().constData(), img.width(), img.height(), argv[3]);
    return 0;
}
