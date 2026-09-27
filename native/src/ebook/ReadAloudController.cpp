#include "ReadAloudController.h"
#include "ReadAloudTarget.h"

#include "../core/AppBrand.h"
#include "../core/AppPaths.h"
#include "../core/Settings.h"
#include "../core/SpeedStore.h"

#include <QDebug>
#include <QLocale>
#include <QSettings>
#include <QTimer>

#include <cmath>

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

    // The sleep timer's clock and its fade (issue #145). The tick only drives the volume and the minutes the
    // controls show; it never stops narration itself - stopping is decided at utterance boundaries, below.
    mono_.start();
    sleepTick_ = new QTimer(this);
    sleepTick_->setInterval(250);
    connect(sleepTick_, &QTimer::timeout, this, &ReadAloudController::onSleepTick);
}

// ---- Voices ------------------------------------------------------------------------------------------------

// Plumbing only. WHICH voices are offered and which is picked is ReadAloud::chooseVoices (pure, and pinned by
// probe_readaloud since #283): this converts the engine's voices into its plain records, asks, and maps the
// answer back onto the same voice objects.
void ReadAloudController::loadVoices()
{
    const QList<QVoice> installed = tts_->availableVoices();
    QVector<ReadAloud::VoiceOption> options;
    options.reserve(installed.size());
    for (const QVoice& v : installed) options.append(ReadAloud::VoiceOption{ v.name(), v.locale() });

    const QString pref = target_ ? target_->raPreferredLanguage() : QString();
    const ReadAloud::VoiceChoice choice =
        ReadAloud::chooseVoices(options, pref, store().value(kVoiceKey).toString());

    voices_.clear();
    for (int i : choice.offered) voices_.append(installed[i]);
    voiceIdx_ = choice.selected;
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
    // Leaving the book ends its sleep timer (#145). stop() above already did when narration was running; said
    // again here so the rule does not depend on it having been.
    if (sleep_.armed()) { sleep_.bookLeft(); qInfo().noquote() << "readaloud.sleep: disarmed (left the book)"; }
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
    // Any stop ends the session's sleep timer (#145) - the user's own included - and hands the engine back its
    // own volume. Before the early return, so no path out of narration can leave a timer armed behind it.
    if (sleep_.armed())
    {
        qInfo().noquote() << QStringLiteral("readaloud.sleep: disarmed (narration stopped, %1 s narrated)")
                                 .arg(sleep_.narrated(nowSec()), 0, 'f', 3);
        sleep_.narrationStopped();
    }
    sleepTick_->stop();
    applySleepVolume(1.0);
    shownMinutes_ = -2;

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

    // A boundary, and the sleep timer's (#145) one place to stop: Qt emits this only when nothing is being
    // spoken - either the engine was idle when the utterance was queued, or it has just finished the previous
    // one and the next has not started (QTextToSpeechPrivate::updateState hands the engine the next text only
    // AFTER this signal returns, and hands it nothing if a slot stopped it). So stopping here never cuts a
    // word. Asked BEFORE the id is mapped, because every emission is a boundary whatever id it carries.
    if (sleepStopAtBoundary(target_ ? target_->raChapterIndex() : -1)) return;

    const int idx = first_ + int(id);
    if (sleep_.armed())
    {
        // While a timer is armed, every boundary is logged with the session clock: the evidence that a stop lands
        // BETWEEN utterances, and how long each one ran.
        utterStarted_ = sleep_.narrated(nowSec());
        qInfo().noquote() << QStringLiteral("readaloud.sleep: boundary - utterance %1 of %2 (chapter %3, engine id %4) at %5 s narrated")
                                 .arg(idx + 1).arg(utts_.size())
                                 .arg(target_ ? target_->raChapterIndex() : -1)
                                 .arg(qint64(id))
                                 .arg(utterStarted_, 0, 'f', 3);
    }
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
    // A drained engine is a boundary too, so a due sleep timer (#145) stops here rather than re-filling.
    if (sleepStopAtBoundary(target_ ? target_->raChapterIndex() : -1)) return;
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
        // Crossing into the next spine item: End of chapter (#145) stops HERE, the reader already standing at
        // the start of the next chapter (raGotoChapter loads it there and persists it), before a word of it is
        // spoken. A due minute timer stops at this boundary too.
        if (sleepStopAtBoundary(next)) return;
        if (planCurrentChapter()) { speakFrom(0); notifyChanged(); return; }
        // else: nothing to say in this one — keep walking.
    }
    stop();   // the end of the book
}

void ReadAloudController::notifyChanged()
{
    syncSleepClock();   // every state change passes through here, so the sleep clock can never disagree with it
    if (target_) target_->raNarrationChanged();
    emit changed();
}

// ---- The sleep timer (issue #145) -------------------------------------------------------------------------------
// The decision is ReadAloud::SleepSession's (pure, probed with a fake clock in probe_readaloud); everything below
// is plumbing: which clock, when to ask, and what a "yes" does to the engine.

double ReadAloudController::nowSec() const { return double(mono_.elapsed()) / 1000.0; }

bool ReadAloudController::armSleep(const SleepTimer::Timer& t)
{
    if (!active_ || !target_) return false;
    const double now = nowSec();
    if (!sleep_.arm(t, now, target_->raChapterIndex(), paused_))
    {
        disarmSleep();   // Off (or a zero Custom) over an armed timer: turn it off, fade and all
        return false;
    }
    utterStarted_ = 0.0;
    shownMinutes_ = -2;
    applySleepVolume(1.0);   // a re-arm lifts a fade the old timer had applied
    sleepTick_->start();
    if (t.mode == SleepTimer::Mode::EndOfChapter)
        qInfo().noquote() << QStringLiteral("readaloud.sleep: armed end-of-chapter (chapter %1)")
                                 .arg(target_->raChapterIndex());
    else
        qInfo().noquote() << QStringLiteral("readaloud.sleep: armed %1 min of narration (chapter %2%3)")
                                 .arg(t.minutes).arg(target_->raChapterIndex())
                                 .arg(paused_ ? QStringLiteral(", paused - the clock starts on resume") : QString());
    notifyChanged();
    return true;
}

void ReadAloudController::disarmSleep()
{
    const bool was = sleep_.armed();
    sleep_.disarm();
    sleepTick_->stop();
    applySleepVolume(1.0);
    shownMinutes_ = -2;
    if (was) qInfo().noquote() << "readaloud.sleep: disarmed (turned off)";
    notifyChanged();
}

int ReadAloudController::sleepMinutesLeft() const
{
    if (sleep_.mode() != SleepTimer::Mode::Minutes) return -1;
    return int(std::ceil(sleep_.secondsLeft(nowSec(), -1.0) / 60.0));
}

bool ReadAloudController::sleepStopAtBoundary(int nextChapter)
{
    if (!sleep_.armed()) return false;
    const double now = nowSec();
    if (!sleep_.stopAtBoundary(now, nextChapter)) return false;
    qInfo().noquote() << QStringLiteral("readaloud.sleep: fired at an utterance boundary (%1, %2 s narrated, next chapter %3)")
                             .arg(sleep_.mode() == SleepTimer::Mode::EndOfChapter ? QStringLiteral("end of chapter")
                                                                                  : QStringLiteral("minutes"))
                             .arg(sleep_.narrated(now), 0, 'f', 3)
                             .arg(nextChapter);
    stop();                 // the existing contract: the reader stays where narration reached, position saved
    // One more purge on the next turn. When this boundary was an ENQUEUE onto an idle engine (a restart - skip,
    // voice, speed), Qt emits aboutToSynthesize and then calls the engine's say() regardless of what the slot
    // did, so the stop above would be followed by one stray utterance. Anywhere else this finds the engine idle
    // and does nothing.
    QMetaObject::invokeMethod(this, [this] {
        if (!active_) tts_->stop(QTextToSpeech::BoundaryHint::Immediate);
    }, Qt::QueuedConnection);
    emit sleepStopped();
    return true;
}

void ReadAloudController::syncSleepClock()
{
    if (!sleep_.armed()) return;
    const double now = nowSec();
    if (active_ && !paused_) sleep_.resume(now);
    else                     sleep_.pause(now);
}

void ReadAloudController::onSleepTick()
{
    if (!sleep_.armed() || !active_) { sleepTick_->stop(); return; }
    const double now = nowSec();
    // End of chapter's seconds-left is an estimate from what is still to be said; a minute timer's is exact,
    // and the session ignores this argument for it.
    const double est = sleep_.mode() == SleepTimer::Mode::EndOfChapter
                           ? ReadAloud::chapterSecondsLeft(utts_, current_, sleep_.narrated(now) - utterStarted_, speed_)
                           : -1.0;
    applySleepVolume(sleep_.gain(now, est));
    const int mins = sleepMinutesLeft();
    if (mins != shownMinutes_)
    {
        if (mins == 0)
            qInfo().noquote() << QStringLiteral("readaloud.sleep: minutes are up at %1 s narrated - finishing the utterance")
                                     .arg(sleep_.narrated(now), 0, 'f', 3);
        shownMinutes_ = mins;
        notifyChanged();    // the controls show the minutes left; re-label only when that number moves
    }
}

// Qt's volume, scaled by the fade. The engine's own volume is captured the first time a fade goes below 1 and
// written back when it returns to 1 (disarm, stop, re-arm), so the fade is a transient over the level the engine
// had, never a replacement for it. An engine without a volume control ignores setVolume, and then the timer
// simply stops without fading.
void ReadAloudController::applySleepVolume(double gain)
{
    if (gain >= 1.0)
    {
        if (baseVolume_ >= 0.0) tts_->setVolume(baseVolume_);
        baseVolume_ = -1.0;
        appliedGain_ = 1.0;
        return;
    }
    if (baseVolume_ < 0.0) baseVolume_ = tts_->volume();
    if (std::abs(gain - appliedGain_) < 0.01) return;
    appliedGain_ = gain;
    tts_->setVolume(baseVolume_ * gain);
    qInfo().noquote() << QStringLiteral("readaloud.sleep: fade gain %1").arg(gain, 0, 'f', 2);
}
