// Verifies the in-app theme path end to end: ThemeEngine loads a theme from disk and renders it through the
// qrc-embedded QML in a QQuickView (exactly what the app does), then grabs a frame to PNG.
// Usage: probe_theme2 <themeDir> <out.png> [sampleImage] [startIndex]
#include "ThemeEngine.h"
#include <QApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QEventLoop>
#include <QTimer>
#include <QImage>
#include <QFileInfo>
#include <cstdio>

int main(int argc, char** argv)
{
    qputenv("QT_QUICK_BACKEND", "software");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software); // match the app
    QApplication app(argc, argv);
    if (argc < 3) { std::fprintf(stderr, "usage: probe_theme2 <themeDir> <out.png> [sampleImage]\n"); return 2; }
    const QString themeDir = QString::fromLocal8Bit(argv[1]);
    const QString outPng   = QString::fromLocal8Bit(argv[2]);
    const QString sampleImg = argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QString();

    // A stand-in catalog (the app feeds real recents/catalogs here).
    const char* names[] = { "Movies", "TV", "Music", "Books", "Comics", "Live TV", "Live Sports", "Games", "Podcasts", "Audiobooks", "Manga", "Retro" };
    const char* cols[]  = { "#C0392B", "#2980B9", "#8E44AD", "#3E8E7E", "#E07A2E", "#E23B3B", "#2EA043", "#7A5BD0", "#C44ED1", "#5B7FE0", "#CE5797", "#16A085" };
    QVariantList items;
    for (int i = 0; i < 12; ++i)
    {
        QVariantMap m;
        m["title"] = names[i];
        m["accent"] = cols[i];
        m["subtitle"] = QStringLiteral("%1 items").arg((i + 3) * 17);
        m["rating"] = (i % 5) / 5.0 + 0.2;
        m["overview"] = QStringLiteral("A short description of %1 goes here. The detail view binds to "
                                       "selected.overview and wraps it; themers control the layout entirely.").arg(names[i]);
        if (i < 3 && !sampleImg.isEmpty()) m["image"] = sampleImg;
        items << m;
    }
    QVariantMap system; system["name"] = QFileInfo(themeDir).fileName();

    QWidget* w = ThemeEngine::buildView(themeDir, items, system, nullptr);
    QQuickItem* root = ThemeEngine::rootItem(w);
    if (!root) { std::fprintf(stderr, "no root item (QML failed to load)\n"); return 1; }
    QQuickWindow* win = root->window(); // the QQuickView embedded by buildView
    if (!win) { std::fprintf(stderr, "no QQuickWindow for root\n"); return 1; }

    const int rw = qEnvironmentVariableIntValue("PROBE_W"), rh = qEnvironmentVariableIntValue("PROBE_H");
    win->resize(rw > 0 ? rw : 1280, rh > 0 ? rh : 720); // PROBE_W/H override -> verify small-size scaling
    // Optional start selection (argv[4]) - verifies navigation moves the carousel + bound info.
    if (argc >= 5) root->setProperty("currentIndex", QString::fromLocal8Bit(argv[4]).toInt());
    // PROBE_VIEW selects which theme view to render (e.g. "detail") - verifies per-view theming.
    if (!qEnvironmentVariable("PROBE_VIEW").isEmpty())
        root->setProperty("currentView", qEnvironmentVariable("PROBE_VIEW"));

    win->show();
    { QEventLoop loop; QTimer::singleShot(500, &loop, &QEventLoop::quit); loop.exec(); }

    const QImage img = win->grabWindow();
    if (img.isNull() || !img.save(outPng)) { std::fprintf(stderr, "grab/save failed\n"); return 1; }
    std::printf("OK: theme dir \"%s\" rendered %dx%d -> %s\n",
                themeDir.toUtf8().constData(), img.width(), img.height(), outPng.toUtf8().constData());
    return 0;
}
