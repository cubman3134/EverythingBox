// A SERVER BOOK'S WHOLE-BOOK POSITION BAR (issue #197, increment 3) — MainWindow's half.
//
// #218 gave a multi-file audiobook one position bar for the whole book, and it did so for the two sources
// that could describe their parts: a local book (tag durations) and a torrent release (part sizes, priced by
// one measured duration). An Audiobookshelf book was left per-part, because its play session gives track
// DURATIONS and BookTimeline read only sizes. BookTimeline now takes durations as an exact seed, and
// openAbsItem passes them (Abs::bookParts), so the bar and the time readout span the book.
//
// THE ONE THING THIS FILE ADDS IS THE CROSSING. #218's bar may not be dragged out of the part that is
// playing, and the reason is stated in BookTimeline.h: crossing means minting the target part's link, which
// for a torrent release is a debrid resolve of the whole release and can take a minute. None of that is true
// of a server book — AbsClient::partStreamUrl builds the link locally from the play session it already holds,
// with no round trip at all — so for one of those a seek lands where the listener pointed: in the part the
// point falls in, at the offset inside it.
//
// HOW IT CROSSES is the mechanism the audiobook chapter list (#139) and a channel's join-in-progress already
// use: jump the queue to the part (PlaybackSession::playIndex — the same door a queue row or Next uses, so
// the part is minted at the playRequested choke point exactly as it would be if it had been reached), then
// place the start inside it (overrideResumeSeek), which onDuration applies once mpv knows the part's length.
// The resume hook's answer for that part (the server's "the top of it") is replaced, which is the point.
//
// PROGRESS IS UNCHANGED. The report the server receives is already in BOOK seconds — installAbsProgressHooks
// turns a position in part k into track k's startOffset plus that position — and a crossing is just a part
// change followed by a position, so the server hears the landing point in its own time base. The server
// remains the only resume owner (#197's rule, 76fc228f); nothing here writes a resume mark.
#include "MainWindow.h"

#include "../core/AbsClient.h"
#include "../core/AppPaths.h"
#include "../media/PlaybackSession.h"
#include "../video/MpvWidget.h"

#include <QDateTime>
#include <QFile>
#include <QtGlobal>

namespace {
// The same one-line append to <app>/stream_debug.log that MainWindow.cpp's file-static mwLog does, copied
// for MainWindowOpenFail.cpp's reason: lifting it out would touch that file for nothing else.
void atLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                 + QStringLiteral("\n")).toUtf8());
}
} // namespace

bool MainWindow::absBookPlaying() const
{
    // THE GUARD IS THE ENTRY THAT IS PLAYING, not a flag — currentChapters()'s rule, for its reason:
    // absBookId_ is never cleared, so the only question that cannot be left stale is "does the thing playing
    // right now belong to that book".
    if (absBookId_.isEmpty() || !session_) return false;
    const int idx = session_->currentIndex();
    if (idx < 0 || idx >= session_->count()) return false;
    return AbsSupply::bookIdOf(session_->trackAt(idx)) == absBookId_;
}

bool MainWindow::absBookSeek(double bookSeconds)
{
    if (!bookScale() || !absBookPlaying()) return false;
    const BookTimeline::Landing l = bookTimeline_.land(bookSeconds);
    if (l.part < 0 || l.part >= session_->count()) return false;

    // Inside the part in hand: an ordinary seek, exactly what the clamp would have done.
    if (l.part == session_->currentIndex())
    {
        player_->setPosition(l.within);
        return true;
    }

    // Another part. onDuration applies a start point only when it is more than a second in and more than
    // five seconds short of the part's end (a near-finished file starts fresh — the resume rule). A seek is
    // not a resume, so a landing inside that last stretch is pulled back just clear of it rather than being
    // thrown to the part's top; everywhere else the landing is exact.
    const double len = bookTimeline_.lengthOf(l.part);
    const double within = len > 7.0 ? qMin(l.within, len - 6.0) : l.within;
    atLog(QStringLiteral("audiobook: seek to %1 s of the book — part %2/%3, %4 s in")
              .arg(bookSeconds, 0, 'f', 1).arg(l.part + 1).arg(session_->count()).arg(within, 0, 'f', 1));
    session_->playIndex(l.part);
    session_->overrideResumeSeek(within);
    return true;
}
