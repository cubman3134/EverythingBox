#include "ReadAloudController.h"
#include "ReadAloudTarget.h"

#include "../core/AppBrand.h"
#include "../core/AppPaths.h"
#include "../core/Settings.h"
#include "../core/SpeedStore.h"

#include <QLocale>
#include <QSettings>

namespace
{
// The one ini every store in this app shares (Settings, SpeedStore, the reader's own per-book keys).
QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// The chosen voice is a DEVICE preference, not content's: which of this machine's voices you like says nothing
// about the book, and the voices on another device are different ones entirely. So it lives under a plain
// device-local key, unlike the speed beside it (which is #140's per-book, per-item-SYNCED memory).
const QString kVoiceKey = QStringLiteral("readaloud/voice");
} // namespace

bool ReadAloudController::engineAvailable()
{
    return !QTextToSpeech::availableEngines().isEmpty();
}

ReadAloudController::ReadAloudController(ReadAloudTarget* target, QObject* parent)
    : QObject(parent), target_(target)
{
    tts_ = new QTextToSpeech(this);
    connect(tts_, &QTextToSpeech::aboutToSynthesize, this, &ReadAloudController::onAboutToSynthesize);
    connect(tts_, &QTextToSpeech::stateChanged, this, &ReadAloudController::onStateChanged);
    loadVoices();
}

// ---- Voices ------------------------------------------------------------------------------------------------

// Offer the voices for the preferred language when there ARE any, and every voice otherwise. Falling back to
// all rather than to none is the important half: a book whose language has no installed voice is still worth
// hearing in another one, and a picker that offers nothing looks broken.
void ReadAloudController::loadVoices()
{
    voices_.clear();
    const QString pref = target_ ? target_->raPreferredLanguage() : QString();
    if (!pref.isEmpty())
    {
        const QLocale want(pref);
        for (const QVoice& v : tts_->availableVoices())
            if (v.locale().language() == want.language()) voices_.append(v);
    }
    if (voices_.isEmpty()) voices_ = tts_->availableVoices();

    // Restore the stored pick BY NAME: a voice's index moves when the system gains or loses one, and resuming
    // on "whatever is third today" is how a setting quietly stops meaning anything.
    voiceIdx_ = 0;
    const QString stored = store().value(kVoiceKey).toString();
    if (!stored.isEmpty())
        for (int i = 0; i < voices_.size(); ++i)
            if (voices_[i].name() == stored) { voiceIdx_ = i; break; }
}

QStringList ReadAloudController::voiceNames() const
{
    QStringList out;
    out.reserve(voices_.size());
    for (const QVoice& v : voices_) out << v.name();
    return out;
}

void ReadAloudController::applyVoice()
{
    if (voiceIdx_ >= 0 && voiceIdx_ < voices_.size()) tts_->setVoice(voices_[voiceIdx_]);
}

void ReadAloudController::cycleVoice()
{
    if (voices_.size() < 2) return;
    voiceIdx_ = (voiceIdx_ + 1) % voices_.size();
    store().setValue(kVoiceKey, voices_[voiceIdx_].name());
    store().sync();
    applyVoice();
    // Re-speak the paragraph from its start rather than letting the change surface two paragraphs later (the
    // look-ahead has already queued them in the OLD voice). A control that appears to do nothing gets pressed
    // again, and again.
    if (active_ && current_ >= 0) speakFrom(current_);
    notifyChanged();
}

// ---- Speed (issue #140's per-book memory, shared with the audiobook) -----------------------------------------

void ReadAloudController::applySpeed()
{
    tts_->setRate(ReadAloud::engineRateForSpeed(speed_));
}

void ReadAloudController::setSpeed(double s)
{
    if (s <= 0.0) return;
    speed_ = s;
    if (target_)
    {
        const QString key = target_->raBookKey();
        if (!key.isEmpty()) SpeedStore::setForItem(key, s);   // the SAME record the audiobook writes
    }
    applySpeed();
    if (active_ && current_ >= 0) speakFrom(current_);        // audible now, for the cycleVoice reason
    notifyChanged();
}

void ReadAloudController::cycleSpeed() { setSpeed(ReadAloud::nextSpeedStep(speed_)); }

// ---- Transport ----------------------------------------------------------------------------------------------

bool ReadAloudController::planCurrentChapter()
{
    utts_ = ReadAloud::plan(target_->raChapterText());
    return !utts_.isEmpty();
}

// The speed is the book's, resolved exactly as the player resolves an item's: the stored per-item value when
// there is one, else the global default. Reading is never music, so the music clamp does not apply.
void ReadAloudController::resolveSpeedForBook()
{
    if (!target_) return;
    const QString key = target_->raBookKey();
    const double stored = key.isEmpty() ? 0.0 : SpeedStore::storedForItem(key);
    speed_ = SpeedStore::speedForItem(stored, Settings::defaultPlaybackSpeed(), /*isMusic*/ false);
    applySpeed();
}

void ReadAloudController::adoptBook()
{
    if (active_) stop();
    resolveSpeedForBook();
    loadVoices();
    applyVoice();
    notifyChanged();
}

void ReadAloudController::start()
{
    if (!target_ || active_) return;

    resolveSpeedForBook();
    loadVoices();
    applyVoice();

    if (!planCurrentChapter())
    {
        // A chapter with nothing to say (a full-page image, a blank divider): walk forward rather than
        // reporting a book that will not speak.
        active_ = true;
        advanceChapterOrStop();
        return;
    }

    const int i = ReadAloud::indexForOffset(utts_, target_->raCurrentOffset());
    active_ = true;
    paused_ = false;
    speakFrom(i < 0 ? 0 : i);
    notifyChanged();
}

void ReadAloudController::stop()
{
    if (!active_ && tts_->state() == QTextToSpeech::Ready) return;
    active_ = false;
    restarting_ = true;
    tts_->stop(QTextToSpeech::BoundaryHint::Immediate);
    restarting_ = false;
    current_ = -1;
    paused_ = false;
    utts_.clear();
    first_ = queued_ = spoken_ = 0;
    if (target_) target_->raClearSpoken();   // the highlight goes; the POSITION stays where it reached
    notifyChanged();
}

void ReadAloudController::toggle() { if (active_) stop(); else start(); }

// BoundaryHint::Default, deliberately: asked to pause at a WORD boundary the Windows SAPI back end does
// nothing at all - narration runs on to the end of the book - while Default halts it at once. "Whether a hint
// is honoured depends on the engine" is Qt's own wording, so the portable thing is to let the engine choose
// WHERE and only insist THAT it pauses.
void ReadAloudController::togglePause()
{
    if (!active_) return;
    if (paused_) { tts_->resume(); paused_ = false; }
    else         { tts_->pause(QTextToSpeech::BoundaryHint::Default); paused_ = true; }
    notifyChanged();
}

// Paragraph back / forward — the reader's twin of #140's jump controls, at the granularity narration actually
// has. Clamped to the chapter: crossing a chapter boundary backwards would have to re-divide the previous
// chapter and land on its LAST utterance, which the chapter-advance path below does not need and this
// increment does not do.
void ReadAloudController::skip(int delta)
{
    if (!active_ || utts_.isEmpty() || delta == 0) return;
    const int t = qBound(0, (current_ < 0 ? 0 : current_) + delta, int(utts_.size()) - 1);
    speakFrom(t);
    notifyChanged();
}

// ---- The queue ------------------------------------------------------------------------------------------------

void ReadAloudController::speakFrom(int index)
{
    if (utts_.isEmpty()) return;
    restarting_ = true;
    if (paused_) tts_->resume();   // a paused engine must not carry its pause into the queue we are about to fill
    tts_->stop(QTextToSpeech::BoundaryHint::Immediate);   // clears the engine's queue, resetting its ids to 0
    restarting_ = false;
    paused_ = false;

    first_ = qBound(0, index, int(utts_.size()) - 1);
    queued_ = first_;
    spoken_ = 0;
    current_ = -1;
    pump();
}

void ReadAloudController::pump()
{
    while (queued_ < utts_.size() && (queued_ - first_ - spoken_) < kLookahead)
        tts_->enqueue(utts_[queued_++].text);
}

void ReadAloudController::onAboutToSynthesize(qsizetype id)
{
    if (!active_) return;
    const int idx = first_ + int(id);
    if (idx < 0 || idx >= utts_.size()) return;

    current_ = idx;
    spoken_  = int(id) + 1;
    if (target_) target_->raShowSpoken(utts_[idx].start, utts_[idx].end);
    pump();               // keep the look-ahead full so the next paragraph starts without a gap
    notifyChanged();
}

void ReadAloudController::onStateChanged(QTextToSpeech::State s)
{
    if (!active_ || restarting_) return;
    if (s == QTextToSpeech::Error) { stop(); return; }
    if (s != QTextToSpeech::Ready) { notifyChanged(); return; }

    // Ready with nothing left to hand over means this chapter is finished.
    if (queued_ >= utts_.size()) { advanceChapterOrStop(); return; }

    // Ready with more to say means the engine drained faster than the look-ahead refilled it (a very short
    // paragraph, or a slow signal). Re-base on what is left and carry on rather than stopping mid-chapter.
    first_ = queued_;
    spoken_ = 0;
    pump();
}

void ReadAloudController::advanceChapterOrStop()
{
    if (!target_) { stop(); return; }
    const int count = target_->raChapterCount();
    for (int next = target_->raChapterIndex() + 1; next < count; ++next)
    {
        if (!target_->raGotoChapter(next)) break;
        if (planCurrentChapter()) { speakFrom(0); notifyChanged(); return; }
        // else: nothing to say in this one — keep walking.
    }
    stop();   // the end of the book
}

void ReadAloudController::notifyChanged()
{
    if (target_) target_->raNarrationChanged();
    emit changed();
}
