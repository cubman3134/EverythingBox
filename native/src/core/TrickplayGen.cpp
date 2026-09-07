#include "TrickplayGen.h"

#include "AppPaths.h"
#include "Settings.h"

#include <QAtomicInt>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QThread>
#include <QVector>

#include <mpv/client.h>

namespace
{
// The shipped sheet. One frame every 10 s at 320x180, 5x5 to a grid — 250 s of film per JPEG, which is the
// standard trickplay/BIF shape and is what makes a scrub cost one already-decoded grid and a blit.
constexpr int kIntervalMs = 10000;
constexpr int kTileW      = 320;
constexpr int kTileH      = 180;
constexpr int kCols       = 5;
constexpr int kRows       = 5;
constexpr int kJpegQuality = 80;

// Write to a sibling ".part" and rename into place, so a reader never sees half a file and an interrupted
// run leaves whole grids or nothing. QFile::rename will not replace on Windows, hence the remove first.
bool writeAtomically(const QString& path, const QByteArray& bytes)
{
    const QString tmp = path + QStringLiteral(".part");
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (f.write(bytes) != bytes.size()) { f.close(); QFile::remove(tmp); return false; }
    f.close();
    QFile::remove(path);
    if (!QFile::rename(tmp, path)) { QFile::remove(tmp); return false; }
    return true;
}

bool saveImageAtomically(const QString& path, const QImage& img)
{
    const QString tmp = path + QStringLiteral(".part");
    if (!img.save(tmp, "JPG", kJpegQuality)) { QFile::remove(tmp); return false; }
    QFile::remove(path);
    if (!QFile::rename(tmp, path)) { QFile::remove(tmp); return false; }
    return true;
}
} // namespace

// ---------------------------------------------------------------------------------------------------
// The worker. Everything below this line runs on TrickplayGen's own thread: the mpv handle is created and
// destroyed there, the images are built there, and the eviction sweep runs there.
// ---------------------------------------------------------------------------------------------------
class TrickplayWorker : public QObject
{
    Q_OBJECT
public:
    // Read by the worker between every frame, written by the GUI thread. The only shared state there is.
    QAtomicInt cancel{ 0 };

public slots:
    void generate(const QString& path, qint64 boundBytes)
    {
        const QString dir = TrickplayGen::itemDirFor(path);
        if (dir.isEmpty()) { emit done(path); return; }
        if (!QDir().mkpath(dir))  { emit done(path); return; }

        const QFileInfo fi(path);
        const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
        const qint64 size  = fi.size();

        mpv_handle* m = mpv_create();
        if (!m) { emit done(path); return; }
        // A thumbnailer, not a player: no audio device, no subtitle renderer, no OSD, no terminal, and the
        // scale filter in front so mpv hands back the tile at its final size (see the header's measurement).
        mpv_set_option_string(m, "vo", "null");
        mpv_set_option_string(m, "audio", "no");
        mpv_set_option_string(m, "sub", "no");
        mpv_set_option_string(m, "osd-level", "0");
        mpv_set_option_string(m, "pause", "yes");
        mpv_set_option_string(m, "keep-open", "yes");
        mpv_set_option_string(m, "hr-seek", "yes");
        mpv_set_option_string(m, "terminal", "no");
        mpv_set_option_string(m, "config", "no");        // never read the user's mpv.conf into a batch job
        mpv_set_option_string(m, "load-scripts", "no");
        mpv_set_option_string(m, "network-timeout", "5"); // belt and braces: this path never sees a network url
        if (mpv_initialize(m) < 0) { mpv_terminate_destroy(m); emit done(path); return; }

        const QByteArray vf = QStringLiteral("scale=%1:%2").arg(kTileW).arg(kTileH).toUtf8();
        mpv_set_property_string(m, "vf", vf.constData());

        const QByteArray p8 = QDir::toNativeSeparators(path).toUtf8();
        const char* load[] = { "loadfile", p8.constData(), nullptr };
        if (mpv_command(m, load) < 0) { mpv_terminate_destroy(m); emit done(path); return; }

        double durationSec = 0.0;
        if (!waitLoaded(m, &durationSec) || durationSec <= 0.0)
        { mpv_terminate_destroy(m); emit done(path); return; }

        Trickplay::Layout layout;
        layout.intervalMs = kIntervalMs;
        layout.tileW = kTileW; layout.tileH = kTileH;
        layout.cols  = kCols;  layout.rows  = kRows;
        layout.frameCount = Trickplay::frameCountFor(qint64(durationSec * 1000.0), kIntervalMs);
        const int grids = Trickplay::gridCount(layout);
        if (grids <= 0) { mpv_terminate_destroy(m); emit done(path); return; }

        // Resume: which grids are already on disk. The FIRST hole, not the count — see Trickplay::resumeGrid.
        QVector<bool> present(grids, false);
        for (int g = 0; g < grids; ++g)
            present[g] = QFile::exists(dir + QLatin1Char('/') + Trickplay::gridFileName(g));
        int g = Trickplay::resumeGrid(layout, present);

        Trickplay::Index ix;
        ix.layout       = layout;
        ix.durationMs   = qint64(durationSec * 1000.0);
        ix.sourceMtime  = mtime;
        ix.sourceSize   = size;
        ix.sourceName   = fi.fileName();

        const QString tile = dir + QStringLiteral("/frame.tmp.jpg");
        bool cancelled = false;

        for (; g < grids; ++g)
        {
            if (cancel.loadAcquire()) { cancelled = true; break; }

            const int n = Trickplay::framesInGrid(layout, g);
            QImage sheet(Trickplay::gridPixelWidth(layout, g), Trickplay::gridPixelHeight(layout, g),
                         QImage::Format_RGB32);
            if (sheet.isNull()) break;
            sheet.fill(Qt::black);
            QPainter painter(&sheet);

            bool gridOk = true;
            for (int i = 0; i < n; ++i)
            {
                if (cancel.loadAcquire()) { cancelled = true; gridOk = false; break; }
                const int frame = g * layout.perGrid() + i;
                const double at = double(Trickplay::frameTimeMs(layout, frame)) / 1000.0;
                if (!seekExact(m, at)) { gridOk = false; break; }
                QFile::remove(tile);
                const QByteArray t8 = QDir::toNativeSeparators(tile).toUtf8();
                const char* shot[] = { "screenshot-to-file", t8.constData(), "video", nullptr };
                if (mpv_command(m, shot) < 0) { gridOk = false; break; }
                QImage img(tile);
                if (img.isNull()) { gridOk = false; break; }
                if (img.width() != layout.tileW || img.height() != layout.tileH)
                    img = img.scaled(layout.tileW, layout.tileH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                const Trickplay::Tile pos = Trickplay::tileOfFrame(layout, frame);
                painter.drawImage(Trickplay::tilePixelX(layout, pos), Trickplay::tilePixelY(layout, pos), img);
            }
            painter.end();
            if (!gridOk) break;

            if (!saveImageAtomically(dir + QLatin1Char('/') + Trickplay::gridFileName(g), sheet)) break;
            ix.completeGrids = g + 1;
            writeAtomically(dir + QStringLiteral("/index.json"), Trickplay::writeIndex(ix));
            emit progressed(path);
        }

        QFile::remove(tile);
        mpv_terminate_destroy(m);

        // The sweep runs here, on this thread, whether or not the walk finished — an interrupted item still
        // added bytes. The item just touched is protected from its own sweep.
        sweep(boundBytes, QFileInfo(dir).fileName());
        Q_UNUSED(cancelled);
        emit done(path);
    }

    // Trim the cache to the bound, least recently used first. "Last used" is the sidecar's modification
    // time, which the player touches when it opens an item — so an item you watch stays, and one you have
    // not opened in a year goes, whatever order they were generated in.
    void sweep(qint64 boundBytes, const QString& keepKey)
    {
        const QString root = TrickplayGen::cacheRoot();
        QDir rd(root);
        if (!rd.exists()) return;

        QVector<Trickplay::CacheEntry> entries;
        const QFileInfoList items = rd.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& item : items)
        {
            Trickplay::CacheEntry e;
            e.key = item.fileName();
            const QFileInfo sidecar(item.absoluteFilePath() + QStringLiteral("/index.json"));
            e.lastUsedSecs = sidecar.exists() ? sidecar.lastModified().toSecsSinceEpoch()
                                              : item.lastModified().toSecsSinceEpoch();
            QDir d(item.absoluteFilePath());
            for (const QFileInfo& f : d.entryInfoList(QDir::Files)) e.bytes += f.size();
            entries.push_back(e);
        }

        const QStringList victims = Trickplay::planEviction(entries, boundBytes, keepKey);
        for (const QString& v : victims)
        {
            // Paranoia that costs nothing and is the difference between a cache sweep and a data-loss bug:
            // only ever a directory directly under our own root, named as a key, never a path from anywhere.
            if (v.isEmpty() || v.contains(QLatin1Char('/')) || v.contains(QLatin1Char('\\'))
                || v == QLatin1String(".") || v == QLatin1String(".."))
                continue;
            QDir(root + QLatin1Char('/') + v).removeRecursively();
        }
    }

    // "Last used" for eviction: touch the sidecar. On the worker thread because it is a disk write, and it
    // is the only thing the read side asks of this class.
    void touch(const QString& dir)
    {
        QFile f(dir + QStringLiteral("/index.json"));
        if (f.open(QIODevice::ReadWrite))
            f.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    }

signals:
    void progressed(const QString& path);
    void done(const QString& path);

private:
    bool waitLoaded(mpv_handle* m, double* durationSec)
    {
        for (;;)
        {
            mpv_event* e = mpv_wait_event(m, 20.0);
            if (!e || e->event_id == MPV_EVENT_NONE) return false;      // a file that will not open
            if (e->event_id == MPV_EVENT_END_FILE) return false;
            if (e->event_id == MPV_EVENT_FILE_LOADED) break;
            if (cancel.loadAcquire()) return false;
        }
        return mpv_get_property(m, "duration", MPV_FORMAT_DOUBLE, durationSec) >= 0;
    }

    // Throw away every event mpv already has for us. This is not tidiness, it is the whole correctness of
    // the capture: `loadfile` leaves a PLAYBACK_RESTART of its OWN sitting in the queue, and the first seek
    // then reads that one, decides its seek is finished before it has started, and screenshots the frame it
    // was already on. Every capture after it is one seek behind — so a two-hour film gets a strip that is
    // silently shifted by ten seconds, with each thumbnail captioned with a time it is not of.
    //
    // It was found, not reasoned about: a fixture with the timecode burned into every frame said "0:00"
    // under a thumbnail asked for at 1:00. Nothing else would have shown it — the pictures are all real
    // frames of the real film, and they are all plausible.
    void drain(mpv_handle* m)
    {
        for (;;)
        {
            mpv_event* e = mpv_wait_event(m, 0.0);
            if (!e || e->event_id == MPV_EVENT_NONE) return;
        }
    }

    // Seek and wait for the decoder to be sitting on the new frame. Waiting for PLAYBACK_RESTART rather
    // than screenshotting straight after the command is what stops a grid quietly filling with the frame
    // before the one asked for — and draining first (above) is what makes the restart we wait for OURS.
    bool seekExact(mpv_handle* m, double t)
    {
        drain(m);
        const QByteArray at = QByteArray::number(t, 'f', 3);
        const char* cmd[] = { "seek", at.constData(), "absolute+exact", nullptr };
        if (mpv_command(m, cmd) < 0) return false;
        for (;;)
        {
            mpv_event* e = mpv_wait_event(m, 20.0);
            if (!e || e->event_id == MPV_EVENT_NONE) return false;
            if (e->event_id == MPV_EVENT_END_FILE) return false;
            if (e->event_id == MPV_EVENT_PLAYBACK_RESTART) return true;
            if (cancel.loadAcquire()) return false;
        }
    }
};

// ---------------------------------------------------------------------------------------------------

TrickplayGen::TrickplayGen(QObject* parent) : QObject(parent)
{
    thread_ = new QThread(this);
    thread_->setObjectName(QStringLiteral("trickplay"));
    worker_ = new TrickplayWorker;
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &TrickplayWorker::progressed, this, &TrickplayGen::itemProgressed);
    connect(worker_, &TrickplayWorker::done, this, [this](const QString&) { busy_ = false; pump(); });
    // Below normal: this is a courtesy job. It must never be the reason anything else is slow.
    thread_->start(QThread::LowPriority);
}

TrickplayGen::~TrickplayGen()
{
    worker_->cancel.storeRelease(1);
    thread_->quit();
    // A generous wait: the worker checks the cancel flag between frames, so the longest it can be is one
    // seek plus one screenshot. Terminating a thread holding an mpv handle is not an option.
    thread_->wait(15000);
}

QString TrickplayGen::cacheRoot()
{
    return AppPaths::dataDir() + QStringLiteral("/previews");
}

QString TrickplayGen::itemDirFor(const QString& path)
{
    if (!Trickplay::eligible(path)) return QString();
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) return QString();
    const QString key = Trickplay::cacheKey(fi.absoluteFilePath(),
                                            fi.lastModified().toSecsSinceEpoch(), fi.size());
    return cacheRoot() + QLatin1Char('/') + key;
}

bool TrickplayGen::loadIndex(const QString& path, Trickplay::Index* out, QString* dirOut)
{
    if (!out) return false;
    const QString dir = itemDirFor(path);
    if (dir.isEmpty()) return false;
    QFile f(dir + QStringLiteral("/index.json"));
    if (!f.open(QIODevice::ReadOnly)) return false;
    Trickplay::Index ix;
    if (!Trickplay::readIndex(f.readAll(), &ix)) return false;
    // The directory is already keyed by path+mtime+size, so a stale one is not FOUND rather than being
    // found and rejected. This second check is the belt: a sidecar copied between machines, or a directory
    // hand-edited, still has to describe the file we are actually about to play.
    const QFileInfo fi(path);
    if (ix.sourceMtime != fi.lastModified().toSecsSinceEpoch() || ix.sourceSize != fi.size()) return false;
    // Only the grids that are actually complete may be looked up; the rest of the film has no strip yet and
    // must fall back to today's behaviour rather than to a missing file.
    ix.layout.frameCount = qMin(ix.layout.frameCount, ix.completeGrids * ix.layout.perGrid());
    *out = ix;
    if (dirOut) *dirOut = dir;
    return true;
}

void TrickplayGen::noteUsed(const QString& dir)
{
    if (dir.isEmpty()) return;
    QMetaObject::invokeMethod(worker_, "touch", Qt::QueuedConnection, Q_ARG(QString, dir));
}

void TrickplayGen::request(const QString& path)
{
    if (Settings::previewCacheMb() <= 0) return;          // previews off: nothing is generated, ever
    if (itemDirFor(path).isEmpty()) return;               // not ours to preview, or gone
    pending_ = path;
    pump();
}

void TrickplayGen::setPlaybackBusy(bool busy)
{
    playing_ = busy;
    if (busy) worker_->cancel.storeRelease(1);            // give the decoder back to the player at once
    else      pump();
}

void TrickplayGen::cancelAll()
{
    pending_.clear();
    worker_->cancel.storeRelease(1);
}

void TrickplayGen::pump()
{
    if (playing_ || busy_ || pending_.isEmpty()) return;
    const qint64 bound = qint64(Settings::previewCacheMb()) * 1024 * 1024;
    if (bound <= 0) { pending_.clear(); return; }
    const QString path = pending_;
    pending_.clear();
    busy_ = true;
    worker_->cancel.storeRelease(0);
    QMetaObject::invokeMethod(worker_, "generate", Qt::QueuedConnection,
                              Q_ARG(QString, path), Q_ARG(qint64, bound));
}

#include "TrickplayGen.moc"
