// A libmpv-backed video preview that renders inside the themed QML view. The themed view runs on Qt Quick's
// SOFTWARE backend (so it can coexist with the app's GL video widget), where QML VideoOutput doesn't render.
// So this is the RetroBat/EmulationStation trick: mpv decodes the clip and, via its SOFTWARE render API
// (MPV_RENDER_API_TYPE_SW), hands us each frame in a CPU buffer; we wrap it as a QImage and paint it into the
// scene ourselves — which works on the software backend because we do the blitting.
//
// mpv opens http(s) urls directly, so a scraper's direct video url streams with no pre-download; a cached
// local file (MetaCache) plays instantly and offline. Silent by default (aid=no) so it doesn't fight the
// menu music / the item's own theme song; loops while the item stays selected.
//
// Registered as QML type MMV/MpvPreview; Video.qml creates it at runtime (guarded) only when a playable clip
// exists, and keeps the Ken Burns still underneath until the first frame arrives.
#pragma once
#include <QImage>
#include <QQuickPaintedItem>

struct mpv_handle;
struct mpv_render_context;

class MpvPreview : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged) // true once real frames are on screen
public:
    explicit MpvPreview(QQuickItem* parent = nullptr);
    ~MpvPreview() override;

    QString source() const { return source_; }
    void setSource(const QString& s);
    bool playing() const { return playing_; }

    void paint(QPainter* p) override;

signals:
    void sourceChanged();
    void playingChanged();

private slots:
    void renderFrame(); // pull the current frame from mpv into frame_ (GUI thread)
    void drainEvents(); // drain mpv's event queue

private:
    static void onMpvRedraw(void* ctx); // mpv render thread -> queue renderFrame()
    static void onMpvWakeup(void* ctx); // mpv thread -> queue drainEvents()
    void setPlaying(bool p);

    mpv_handle* mpv_ = nullptr;
    mpv_render_context* rctx_ = nullptr;
    QImage frame_;
    QString source_;
    bool playing_ = false;
};
