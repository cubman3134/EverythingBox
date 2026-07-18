// Verifies the themed video path end to end WITHOUT a scraper/key: MpvPreview (libmpv software-render item)
// decodes a clip and paints real frames into a Qt Quick SOFTWARE-backend scene. Uses mpv's built-in lavfi
// test source so no video file is needed. Passes when the item reports playing and the grabbed frame is a
// real (non-uniform) picture. Prints MPV-PREVIEW-OK; MPV-PREVIEW-FAIL <what> + non-zero on failure.
#include "MpvPreview.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <cstdio>

// True when the image has real variation (a decoded frame), not a single flat colour (nothing rendered).
static bool looksLikePicture(const QImage& img)
{
    if (img.isNull() || img.width() < 8 || img.height() < 8) return false;
    const QRgb first = img.pixel(0, 0);
    int differing = 0, samples = 0;
    for (int y = 0; y < img.height(); y += qMax(1, img.height() / 20))
        for (int x = 0; x < img.width(); x += qMax(1, img.width() / 20))
        {
            ++samples;
            if (img.pixel(x, y) != first) ++differing;
        }
    return samples > 0 && differing * 4 >= samples; // >=25% of sampled pixels differ from the corner
}

int main(int argc, char** argv)
{
    qputenv("QT_QUICK_BACKEND", "software");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software); // match the app's themed view
    QApplication app(argc, argv);
    qmlRegisterType<MpvPreview>("MMV", 1, 0, "MpvPreview");

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(320, 240);

    QQmlComponent comp(view.engine());
    comp.setData("import QtQuick\nimport MMV 1.0\nMpvPreview { anchors.fill: parent }", QUrl());
    auto* root = qobject_cast<QQuickItem*>(comp.create());
    if (!root) { std::fprintf(stderr, "MPV-PREVIEW-FAIL could not create MpvPreview (%s)\n",
                              comp.errorString().toUtf8().constData()); return 1; }
    view.setContent(QUrl(), &comp, root);
    view.show();

    // mpv's lavfi test pattern: a moving colour picture generated inside libav — no file needed.
    root->setProperty("source", QStringLiteral("av://lavfi:testsrc=size=320x240:rate=25"));

    // Spin the event loop until it reports playing (frames flowing) or we time out.
    QElapsedTimer t; t.start();
    bool played = false;
    while (t.elapsed() < 12000)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (root->property("playing").toBool()) { played = true; break; }
    }
    if (!played) { std::fprintf(stderr, "MPV-PREVIEW-FAIL no frames within timeout (mpv/lavfi unavailable?)\n"); return 1; }

    // Let a couple more frames land, then grab and confirm it's a real picture, not a flat fill.
    for (int i = 0; i < 10; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
    const QImage grab = view.grabWindow();
    if (!looksLikePicture(grab)) { std::fprintf(stderr, "MPV-PREVIEW-FAIL grabbed frame is blank/uniform\n"); return 1; }

    // Clearing the source must actually blank the item — and KEEP it blank. mpv's render thread signals a
    // redraw that was queued before the async "stop" lands; rendering it re-painted the previous clip's LAST
    // frame (and flipped `playing` back on), ghosting the old game's video over the next game's artwork in
    // the themed panel. Pump well past that race window and assert nothing came back.
    root->setProperty("source", QString());
    QElapsedTimer ct; ct.start();
    bool ghostPlaying = false;
    while (ct.elapsed() < 1500)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (root->property("playing").toBool()) { ghostPlaying = true; break; }
    }
    const QImage cleared = view.grabWindow();
    if (ghostPlaying || looksLikePicture(cleared))
    {
        std::fprintf(stderr, "MPV-PREVIEW-FAIL stale frame ghosts after source cleared (playing=%d)\n",
                     int(ghostPlaying));
        return 1;
    }

    // A source mpv cannot open must surface `failed` (END_FILE with an error before any frame) — that's what
    // lets the themed ▶ badge disappear instead of promising a trailer forever — and must NOT flip `playing`.
    root->setProperty("source", QStringLiteral("Z:/definitely/not/a/real/clip.mp4"));
    QElapsedTimer ft; ft.start();
    bool sawFailed = false;
    while (ft.elapsed() < 8000)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (root->property("failed").toBool()) { sawFailed = true; break; }
    }
    if (!sawFailed || root->property("playing").toBool())
    {
        std::fprintf(stderr, "MPV-PREVIEW-FAIL dead source not reported (failed=%d playing=%d)\n",
                     int(sawFailed), int(root->property("playing").toBool()));
        return 1;
    }
    // And a new source after a failure starts clean: `failed` resets with the retry.
    root->setProperty("source", QStringLiteral("av://lavfi:testsrc=size=320x240:rate=25"));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (root->property("failed").toBool())
    {
        std::fprintf(stderr, "MPV-PREVIEW-FAIL failed flag not reset on new source\n");
        return 1;
    }

    std::printf("MPV-PREVIEW-OK (%dx%d frame decoded + software-rendered; clear-source stays blank; "
                "dead source reports failed)\n", grab.width(), grab.height());
    return 0;
}
