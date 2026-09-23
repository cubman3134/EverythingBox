// THE CLASSIC SEEK BAR'S ARROW STEP ON A MULTI-PART BOOK (issue #432) — MainWindow's half.
//
// #218 made the classic seek bar span a whole multi-file book, but an arrow press still moved it by the
// bar's own step (PlayerBarNav.h's kSeekStep, 10 of 1000), and the live seek that followed read the bar
// against the PART that was playing (sliderPosition / 1000 * duration_). So a press moved the handle 1% of
// the book while playback went somewhere else inside the part, and neither was a fixed amount of listening.
//
// THE STEP IS THE PLAYER'S OWN SKIP STEP, IN BOOK SECONDS: audioSkipStep(), the configured jump interval
// (Settings::audioJumpSeconds, 30 s unless changed) that ⏪/⏩ on this bar's row and the themed page's
// seekBack/seekFwd verbs already use. It is not a new number. There is no larger seek step anywhere in the
// player, so Page Up / Page Down keep what they do today on every bar: swallowed (PlayerBarNav's Consume).
//
// WHERE A PRESS LANDS is BookTimeline::stepBook, chosen by BookTimeline::stepRuleFor — pure, and pinned by
// probe_booktimeline. For a book whose parts cost nothing to open (local files, an Audiobookshelf book) a
// press may cross into the next or previous part, through bookCrossSeek — the landing #197 built for a
// server book. For a torrent release it may not: crossing means minting the next part's link, a resolve
// of the whole release, and #216 is one of those taking sixty-five seconds and coming back with nothing.
// There the handle stops at the edge of the part that is playing, exactly as #218's drag clamp does.
//
// A single file, or a book with no timeline, is not touched: stepRuleFor answers Proportional and the
// caller keeps the bar's permille step.
#include "MainWindow.h"

#include "../core/RemoteAudiobook.h"
#include "../media/PlaybackSession.h"
#include "../video/MpvWidget.h"

#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>
#include <QVector>
#include <QtGlobal>

namespace {
// Put the handle and the readout on the aim. The slider's signals are held: its sliderMoved handler is the
// DRAG's, which clamps a local book into the part in hand (#218) and would pull a crossing step back to the
// edge — and which would also take the aim as a drag taking over, and drop it.
void showAim(QSlider* bar, QLabel* readout, double aim, double total, const QString& text)
{
    {
        const QSignalBlocker block(bar);
        bar->setSliderPosition(qBound(0, qRound(aim / total * 1000.0), 1000));
    }
    readout->setText(text);
}
} // namespace

bool MainWindow::bookPartsFree() const
{
    if (!session_) return false;
    if (absBookPlaying()) return true;          // a server book: links built locally (#197)
    // A local book's queue holds file paths; every remote release holds part tokens, whose links are minted
    // on arrival. So "not a part token" is "a file on this disk".
    const int i = session_->currentIndex();
    return i >= 0 && i < session_->count() && !RemoteAudiobook::isPartToken(session_->trackAt(i));
}

bool MainWindow::bookStepKey(int delta)
{
    const BookTimeline::StepRule rule = BookTimeline::stepRuleFor(bookScale(), bookPartsFree());
    if (rule == BookTimeline::StepRule::Proportional) return false;

    const int i = session_->currentIndex();
    const double total = bookTimeline_.total();
    QVector<double> parts;
    parts.reserve(bookTimeline_.parts());
    for (int k = 0; k < bookTimeline_.parts(); ++k) parts << bookTimeline_.lengthOf(k);

    // Where this press starts: where the last press left the aim, else where playback is now. The aim, not
    // the bar, because the bar's thousandths are coarser than one step on a long book, and not the player,
    // because a press that has just crossed parts is ahead of a part mpv has not opened yet.
    const double from = bookAim_ >= 0.0 ? bookAim_ : bookTimeline_.elapsed(i, lastPos_);
    bookAim_ = BookTimeline::stepBook(from, delta, audioSkipStep(), total, parts,
                                      rule == BookTimeline::StepRule::BookWithinPart ? i : -1);

    showAim(seek_, time_, bookAim_, total, fmtBook(bookAim_) + QStringLiteral(" / ") + fmtBook(total));
    return true;
}

bool MainWindow::bookStepCommit()
{
    if (bookAim_ < 0.0) return false;
    const double aim = bookAim_;
    bookAim_ = -1.0;
    if (!bookScale()) return false;
    if (bookPartsFree()) return bookCrossSeek(aim);
    player_->setPosition(bookTimeline_.positionWithin(session_->currentIndex(), aim));
    return true;
}

void MainWindow::liveSeekNow()
{
    if (bookAim_ >= 0.0 && bookScale())
    {
        const double aim = bookAim_;
        const bool adjusting = adjustingBar_ == seek_;
        if (!(bookPartsFree() && bookCrossSeek(aim)))
            player_->setPosition(bookTimeline_.positionWithin(session_->currentIndex(), aim));
        // A crossing opens another part, and every open ABANDONS an Adjusting bar (resetSegmentState) — right
        // for a bar that reads a fraction of one file, whose aim means nothing in the next. This bar's aim is
        // in BOOK seconds and the crossing is the press's own doing, so the bar is taken straight back in
        // hand, still on the aim, and the next press carries on from it instead of walking focus off the bar.
        if (adjusting && adjustingBar_ != seek_ && bookScale())
        {
            setBarAdjusting(seek_, true);   // its sliderPressed clears the aim; it is put back here
            bookAim_ = aim;
            const double total = bookTimeline_.total();
            showAim(seek_, time_, aim, total, fmtBook(aim) + QStringLiteral(" / ") + fmtBook(total));
        }
        return;
    }
    player_->setPosition(seek_->sliderPosition() / 1000.0 * duration_);
}
