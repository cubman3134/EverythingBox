// Seek previews on the player's own seek bar (issue #85, increment 1) — MainWindow's half.
//
// One rule governs every function here, and it is the reason the feature is worth having at all: A PREVIEW
// THAT IS NOT THERE MUST BE INVISIBLE. No empty frame, no placeholder, no spinner, no "generating…" notice.
// A file with no sheets scrubs exactly the way it did before this file existed, and every entry point below
// returns quietly the moment it finds nothing to draw. That is why there is no error path in sight: there is
// nothing a user could usefully do about a missing thumbnail, so they are never told about one.
//
// ONE OVERLAY, BOTH LAYOUTS. Video playback is the same page on the classic and the themed home — see
// openVideoPath's own comment, "openVideoPath is VIDEO — keep the classic player page" — so the transport
// bar this hangs off (mediaControls_ / seek_) is the bar both layouts show, and a preview added here appears
// on both by construction rather than by being written twice. The touch scrub in MainWindowGestures.cpp is
// the third surface onto the same seek, and it is fed from the same two calls.
//
// COST AT PLAY TIME is the whole point of the sprite-sheet layout, so it is worth naming what a scrub does:
// one integer divide (Trickplay::tileAt), and — only when the drag crosses a grid boundary, i.e. every 250
// seconds of film — one JPEG decode. Inside a grid it is a QPixmap::copy of one tile out of an image already
// in memory. Nothing is read from disk while the handle is moving within a grid, and nothing is scaled.
#include "MainWindow.h"

#include "../core/AppPaths.h"
#include "../core/Settings.h"
#include "../core/TrickplayGen.h"
#include "../media/PlaybackSession.h"
#include "../video/MpvWidget.h"

#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QPainter>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>

void MainWindow::initTrickplay()
{
    if (trickGen_) return;
    trickGen_ = new TrickplayGen(this);
    // A strip can appear WHILE the file is open on a later viewing (the job runs between plays, and a walk
    // resumed from grid 12 finishes the rest). Re-reading the sidecar on that signal is what lets the
    // preview turn up without the player being reopened — and it is still silent: nothing is said, a
    // thumbnail simply starts appearing where before there was none.
    connect(trickGen_, &TrickplayGen::itemProgressed, this, [this](const QString& path) {
        if (path.isEmpty() || path != trickPath_) return;
        Trickplay::Index ix;
        QString dir;
        if (!TrickplayGen::loadIndex(path, &ix, &dir)) return;
        trickIndex_ = ix;
        trickDir_   = dir;
        // The grid held in memory may now be a SHORT grid that has since been rewritten full — drop it and
        // let the next scrub read the current one.
        trickGrid_ = QPixmap();
        trickGridNo_ = -1;
    });
}

// Called from onDuration, which mpv re-emits for the SAME file, so the first thing this does is notice it
// has already run for this one. Re-arming is not merely wasteful: it would re-read the sidecar and re-queue
// the generator every time mpv restates a length, which on a file being generated is once a second.
void MainWindow::armTrickplay()
{
    const QString incoming = (session_ && session_->mediaIsVideo()) ? segCtx_.localPath : QString();
    if (!incoming.isEmpty() && incoming == trickPath_) return;   // same file, already armed

    // A new file: whatever the last one had is not this one's. Cleared unconditionally and first, so an
    // ineligible open (a stream) cannot leave the previous FILE's strip armed — which would be the one
    // failure mode that shows a picture of the wrong movie.
    trickIndex_ = Trickplay::Index();
    trickDir_.clear();
    trickPath_.clear();
    trickGrid_ = QPixmap();
    trickGridNo_ = -1;
    trickFrameShown_ = -1;
    trickplayHide();

    if (!trickGen_) return;
    if (Settings::previewCacheMb() <= 0) return;          // previews off: not armed, nothing queued

    // The player is busy from here until it is not: the background job gets nothing while a file is open.
    trickGen_->setPlaybackBusy(true);

    // VIDEO ONLY, and a file we own. segCtx_.localPath is the app's existing "the open media is this file on
    // disk" — empty for a stream, an addon-resolved url, an IPTV channel or a debrid link — and it is then
    // put through Trickplay::eligible anyway. Two gates on purpose: the first is where the app already knows
    // the answer, the second is the value that cannot be argued with at a call site.
    if (!session_ || !session_->mediaIsVideo()) return;
    const QString path = segCtx_.localPath;
    if (path.isEmpty() || !Trickplay::eligible(path)) return;

    trickPath_ = path;
    Trickplay::Index ix;
    QString dir;
    if (TrickplayGen::loadIndex(path, &ix, &dir))
    {
        trickIndex_ = ix;
        trickDir_   = dir;
        trickGen_->noteUsed(dir);       // an item you watch is an item eviction keeps
    }

    // Queued whether or not sheets already exist: a partial run resumes at its first missing grid, and a
    // complete one costs a directory listing and stops. It cannot start while the player holds the machine
    // (setPlaybackBusy above); this is the request the STOP below drains.
    trickGen_->request(path);
}

void MainWindow::trickplayIdle()
{
    if (!trickGen_) return;
    // "Never while playback is running" is about the MACHINE, not about the page. Leaving the player while an
    // album keeps playing behind a browse surface (#193 increment 3) is still playback, so the job stays held
    // off: a courtesy job that makes the music stutter has cost more than it gave.
    if (musicPlayingInBackground()) return;
    trickGen_->setPlaybackBusy(false);
    // The file just watched is the one most worth having a strip for next time, so it is asked for again
    // here: the request made at open was cancelled by the player taking the machine.
    if (!trickPath_.isEmpty()) trickGen_->request(trickPath_);
}

void MainWindow::trickplayHide()
{
    if (trickThumb_) trickThumb_->hide();
    trickFrameShown_ = -1;
}

void MainWindow::trickplayShowAt(double seconds)
{
    // Every one of these is "there is nothing to show", and every one of them is silent.
    if (!player_ || !seek_ || !mediaControls_) return;
    if (trickDir_.isEmpty() || !trickIndex_.layout.valid() || trickIndex_.layout.frameCount <= 0) return;

    const Trickplay::Tile tile = Trickplay::tileAt(trickIndex_.layout, qint64(seconds * 1000.0));
    if (!tile.ok()) return;

    // The same frame as last time: the handle moved within one 10-second bucket. Nothing is decoded, nothing
    // is copied and nothing is repainted — which is what makes dragging the bar cost nothing.
    if (tile.frame == trickFrameShown_ && trickThumb_ && trickThumb_->isVisible()) return;

    if (tile.grid != trickGridNo_)
    {
        // The only disk read in the whole scrub path, and it happens once per 250 s of film crossed.
        QPixmap grid;
        if (!grid.load(trickDir_ + QLatin1Char('/') + Trickplay::gridFileName(tile.grid))) return;
        trickGrid_   = grid;
        trickGridNo_ = tile.grid;
    }

    const int x = Trickplay::tilePixelX(trickIndex_.layout, tile);
    const int y = Trickplay::tilePixelY(trickIndex_.layout, tile);
    if (x + trickIndex_.layout.tileW > trickGrid_.width()
        || y + trickIndex_.layout.tileH > trickGrid_.height())
        return;    // a grid that does not match its own sidecar: show nothing rather than a slice of another tile
    const QPixmap frame = trickGrid_.copy(x, y, trickIndex_.layout.tileW, trickIndex_.layout.tileH);

    if (!trickThumb_)
    {
        // A plain child of the video widget, exactly like skipChip_ and gestureLockBtn_ — never a top-level
        // window and never a dialog (the nav-kit rule): it is decoration over the picture, it takes no focus
        // and it is not in any focus ring.
        trickThumb_ = new QLabel(player_);
        trickThumb_->setObjectName(QStringLiteral("trickThumb"));
        trickThumb_->setAttribute(Qt::WA_TransparentForMouseEvents);
        trickThumb_->setFocusPolicy(Qt::NoFocus);
        trickThumb_->setAlignment(Qt::AlignCenter);
        trickThumb_->setStyleSheet(QStringLiteral(
            "QLabel{background:rgba(0,0,0,200);border:1px solid rgba(255,255,255,60);"
            "border-radius:6px;padding:4px;color:#fff;font-size:13px;}"));
    }

    // The picture and the number are composed into ONE pixmap, from the SAME tile, in one place. That is
    // deliberate and it is the fidelity rule of this feature restated as code: a frame and a timestamp that
    // are set separately can be set out of step, and a thumbnail captioned with a second it is not of is
    // worse than no thumbnail — it invites a seek to the wrong place with complete confidence. The time is
    // the frame's CAPTURE time (Trickplay::frameTimeMs), not the position the pointer happens to be at.
    const int capH = 18;
    QPixmap canvas(frame.width(), frame.height() + capH);
    canvas.fill(Qt::transparent);
    {
        QPainter p(&canvas);
        p.drawPixmap(0, 0, frame);
        p.fillRect(0, frame.height(), canvas.width(), capH, QColor(0, 0, 0, 190));
        p.setPen(QColor(255, 255, 255));
        QFont f = p.font();
        f.setPixelSize(12);
        p.setFont(f);
        p.drawText(QRect(0, frame.height(), canvas.width(), capH), Qt::AlignCenter,
                   gestureTimeText(double(Trickplay::frameTimeMs(trickIndex_.layout, tile.frame)) / 1000.0));
    }
    trickThumb_->setPixmap(canvas);

    const int w = canvas.width() + 10;
    const int h = canvas.height() + 10;

    // Centred over the seek handle, sitting just above the transport bar. Measured off the slider's own
    // groove so it tracks the handle rather than the pointer — a preview a few pixels off the thing it
    // describes reads as lag.
    QStyleOptionSlider opt;
    opt.initFrom(seek_);
    opt.minimum = seek_->minimum();
    opt.maximum = seek_->maximum();
    opt.sliderPosition = seek_->sliderPosition();
    opt.sliderValue = seek_->value();
    opt.orientation = Qt::Horizontal;
    const QRect handle = seek_->style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, seek_);
    const QPoint handleCentreInPlayer =
        seek_->mapTo(player_, QPoint(handle.center().x(), 0));
    int px = handleCentreInPlayer.x() - w / 2;
    int py = mediaControls_->mapTo(player_, QPoint(0, 0)).y() - h - 8;
    px = qBound(6, px, qMax(6, player_->width() - w - 6));
    if (py < 6) py = 6;

    trickThumb_->setFixedSize(w, h);
    trickThumb_->move(px, py);
    trickThumb_->raise();
    trickThumb_->show();
    trickFrameShown_ = tile.frame;
}
