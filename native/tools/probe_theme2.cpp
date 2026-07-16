// Verifies the in-app theme path end to end: ThemeEngine loads a theme from disk and renders it through the
// qrc-embedded QML in a QQuickView (exactly what the app does), then grabs a frame to PNG.
// Usage: probe_theme2 <themeDir> <out.png> [sampleImage] [startIndex]
#include "ThemeEngine.h"
#include "MpvPreview.h"
#include <QApplication>
#include <QtQml>
#include <QWidget>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickView>
#include <QQmlError>
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
    qmlRegisterType<MpvPreview>("MMV", 1, 0, "MpvPreview"); // so Video.qml's real-playback path is available
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
        // Exercise the extensible art schema on the first few rows: a title logo, box art, a screenshot reel,
        // a preview clip/track, and a free-form meta fact — so the new image(role/fallback)/gallery/video
        // elements and selected.meta bindings render with real data (PROBE_ART=1 to force even without a sample).
        if (i < 3 && (!sampleImg.isEmpty() || qEnvironmentVariableIntValue("PROBE_ART")))
        {
            const QString s = sampleImg.isEmpty() ? QString() : sampleImg;
            QVariantMap images;
            images["logo"] = QStringList{ s };
            images["box"] = QStringList{ s };
            images["screenshot"] = QStringList{ s, s, s };
            m["images"] = images;
            m["logo"] = s; m["box"] = s;
            // PROBE_MPV=1 -> a real, file-less test clip (mpv lavfi) so the Video element actually plays;
            // PROBE_NOVIDEO=1 -> no clip at all (verify NO play badge); else a bogus url (badge + fallback).
            if (!qEnvironmentVariableIntValue("PROBE_NOVIDEO"))
                m["videos"] = QStringList{ qEnvironmentVariableIntValue("PROBE_MPV")
                    ? QStringLiteral("av://lavfi:testsrc=size=480x360:rate=25")
                    : QStringLiteral("http://x.invalid/trailer.mp4") };
            m["audio"] = QStringList{ QStringLiteral("http://x.invalid/theme.mp3") };
            m["meta"] = QVariantMap{ { QStringLiteral("developer"), QStringLiteral("Probe Studios") } };
        }
        items << m;
    }
    QVariantMap system; system["name"] = QFileInfo(themeDir).fileName();

    QWidget* w = ThemeEngine::buildView(themeDir, items, system, nullptr);
    QQuickItem* root = ThemeEngine::rootItem(w);
    if (!root)
    {
        if (auto* qv = qobject_cast<QQuickView*>(w->property("mmvQuickView").value<QObject*>()))
            for (const QQmlError& e : qv->errors()) std::fprintf(stderr, "QML: %s\n", e.toString().toUtf8().constData());
        std::fprintf(stderr, "no root item (QML failed to load)\n");
        return 1;
    }
    QQuickWindow* win = root->window(); // the QQuickView embedded by buildView
    if (!win) { std::fprintf(stderr, "no QQuickWindow for root\n"); return 1; }

    const int rw = qEnvironmentVariableIntValue("PROBE_W"), rh = qEnvironmentVariableIntValue("PROBE_H");
    win->resize(rw > 0 ? rw : 1280, rh > 0 ? rh : 720); // PROBE_W/H override -> verify small-size scaling
    // XMB themes read `categories` (the horizontal axis) off the root; feed a stand-in set so it renders.
    {
        const char* catNames[] = { "Game", "Video", "Music", "Photo", "TV", "Live", "Books", "Settings" };
        const char* catCols[]  = { "#7A5BD0", "#C0392B", "#8E44AD", "#3E8E7E", "#2980B9", "#E23B3B", "#E07A2E", "#5B6470" };
        QVariantList cats;
        for (int i = 0; i < 8; ++i) cats << QVariantMap{ { "title", catNames[i] }, { "accent", catCols[i] } };
        root->setProperty("categories", cats);
        root->setProperty("catIndex", 0);
    }
    // A rich selected item for the XMB/Triple metadata panel: title logo art, a hero video (real lavfi clip
    // under PROBE_MPV), box art, and provider facts — so the panel's logo/video/facts wiring is exercised.
    {
        QVariantMap sm;
        sm["title"] = QStringLiteral("Chrono Trigger");
        sm["subtitle"] = QStringLiteral("1995");
        sm["type"] = QStringLiteral("game");
        sm["overview"] = QStringLiteral("A band of adventurers travels through time to prevent a catastrophe.");
        if (!sampleImg.isEmpty())
        {
            sm["image"] = sampleImg; sm["box"] = sampleImg; sm["logo"] = sampleImg; sm["hero"] = sampleImg;
            QVariantMap images;
            images["box"] = QStringList{ sampleImg };
            images["logo"] = QStringList{ sampleImg };
            images["screenshot"] = QStringList{ sampleImg, sampleImg };
            sm["images"] = images;
        }
        sm["videos"] = QStringList{ qEnvironmentVariableIntValue("PROBE_MPV")
            ? QStringLiteral("av://lavfi:testsrc=size=480x360:rate=25") : QStringLiteral("http://x.invalid/t.mp4") };
        QVariantList facts;
        facts << QVariantMap{ { "label", "Developer" }, { "value", "Square" } };
        facts << QVariantMap{ { "label", "Genre" }, { "value", "RPG" } };
        facts << QVariantMap{ { "label", "Players" }, { "value", "1" } };
        sm["facts"] = facts;
        root->setProperty("selectedMeta", sm);
    }
    // Optional start selection (argv[4]) - verifies navigation moves the carousel + bound info.
    if (argc >= 5) root->setProperty("currentIndex", QString::fromLocal8Bit(argv[4]).toInt());
    // PROBE_VIEW selects which theme view to render (e.g. "detail") - verifies per-view theming.
    if (!qEnvironmentVariable("PROBE_VIEW").isEmpty())
        root->setProperty("currentView", qEnvironmentVariable("PROBE_VIEW"));

    win->show();
    const int waitMs = qEnvironmentVariableIntValue("PROBE_WAIT") > 0 ? qEnvironmentVariableIntValue("PROBE_WAIT") : 500;
    { QEventLoop loop; QTimer::singleShot(waitMs, &loop, &QEventLoop::quit); loop.exec(); }

    const QImage img = win->grabWindow();
    if (img.isNull() || !img.save(outPng)) { std::fprintf(stderr, "grab/save failed\n"); return 1; }
    std::printf("OK: theme dir \"%s\" rendered %dx%d -> %s\n",
                themeDir.toUtf8().constData(), img.width(), img.height(), outPng.toUtf8().constData());
    return 0;
}
