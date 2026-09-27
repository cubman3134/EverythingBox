#pragma once
// ReadAloudController — the engine-facing half of read-aloud (issue #145). Compiled ONLY when EB_HAVE_TTS is
// on (see native/CMakeLists.txt): without the Qt TextToSpeech module this file is not in the build at all, and
// the reader draws no read-aloud controls.
//
// It owns a QTextToSpeech and nothing else. The decisions — how a chapter divides into utterances, what is
// stripped before an engine sees it, which utterance an offset names — all live in ReadAloud.{h,cpp}, which is
// pure and probed; this class is the part that cannot be probed on a runner with no audio device, so it is
// deliberately kept to plumbing: queue, signal, position, preference.
//
// SEAMLESS FLOW. Utterances are handed to the engine with enqueue() and a look-ahead of two, so the engine
// always has the next paragraph ready before it finishes the current one — the gap a say()-per-paragraph loop
// leaves is exactly the "reading aloud in chunks" tell the issue warns about. aboutToSynthesize(id) says which
// queued item just started, and that is the edge everything hangs off: the highlight moves, the page turns, the
// position is written, and the queue is topped back up.
//
// PAUSE / SKIP granularity is the utterance, which is why the division matters: pause acts on the paragraph
// being spoken, and paragraph back/forward re-start the queue one utterance either way — the reader's twin of
// #140's jump controls.
#include <QObject>
#include <QStringList>
#include <QVector>
#include <QList>
#include <QTextToSpeech>
#include <QVoice>
#include <QElapsedTimer>

#include "ReadAloud.h"
#include "ReadAloudSleep.h"

class QTimer;

class ReadAloudTarget;

class ReadAloudController : public QObject
{
    Q_OBJECT
public:
    // Is read-aloud usable RIGHT NOW: the module is in this build (or this header would not exist) and the
    // platform actually offers an engine. A Windows box always has SAPI; a bare Linux container may have
    // nothing, and offering a control that cannot speak is worse than not offering one.
    static bool engineAvailable();

    explicit ReadAloudController(ReadAloudTarget* target, QObject* parent = nullptr);

    bool active() const { return active_; }
    // Paused is OUR state, not a question put to the engine. Qt's SAPI back end halts the audio on pause()
    // without moving QTextToSpeech::state() to Paused, so reading the engine's enum would leave narration
    // silently stopped while every control still said "Pause" - measured here on Windows, not assumed. What
    // the label has to reflect is what the user asked for, and this is that.
    bool paused() const { return paused_; }
    // The utterance being spoken, or -1. Only meaningful while active.
    int  currentUtterance() const { return current_; }

    // A book was (re)opened into the reader. Re-resolves this book's speed from #140's per-book memory and
    // re-reads the voices, so the controls SHOW what pressing Read aloud would do before it is pressed - the
    // speed control read "1x" on a book stored at 0.5x until narration started, which is a control that lies
    // right up to the moment it stops mattering.
    void adoptBook();

    void start();          // begin at the reader's current position
    void stop();           // end narration; the reader is left on the page it reached
    void toggle();
    void togglePause();
    void skip(int delta);  // paragraph back / forward, within the chapter

    double speed() const { return speed_; }
    void   setSpeed(double s);
    void   cycleSpeed();   // step through ReadAloud::speedSteps(), wrapping

    QStringList voiceNames() const;
    int  voiceIndex() const { return voiceIdx_; }
    void cycleVoice();     // step through the offered voices, wrapping

    // The sleep timer (issue #145, the #140 timer applied to narration). The decision is ReadAloud::SleepSession
    // (pure, probed); this is its clock, its fade and its stop. Armed only while narrating - arming when not
    // narrating, or with Off, returns false and leaves nothing armed. ANY stop disarms it (see stop()).
    bool armSleep(const SleepTimer::Timer& t);
    void disarmSleep();                       // Off from the menu
    bool sleepArmed() const { return sleep_.armed(); }
    SleepTimer::Mode sleepMode() const { return sleep_.mode(); }
    // Whole minutes of narration left on a minute timer (rounded up), or -1 for End of chapter / not armed.
    int  sleepMinutesLeft() const;

signals:
    void changed();        // active/paused/paragraph/speed/voice/sleep timer moved
    void sleepStopped();   // the sleep timer, not the user, just ended narration

private slots:
    void onAboutToSynthesize(qsizetype id);
    void onSayingWord(const QString& word, qsizetype id, qsizetype start, qsizetype length);
    void onStateChanged(QTextToSpeech::State s);

private:
    void loadVoices();             // gather the offered voices and restore the stored pick
    void resolveSpeedForBook();    // this book's speed, resolved exactly as the player resolves an item's
    void applySpeed();
    void applyVoice();
    void pump();                   // top the engine's queue back up to the look-ahead
    void speakFrom(int index);     // restart the queue at an utterance
    bool planCurrentChapter();     // (re)divide the chapter the reader is on; false when it has nothing to say
    void advanceChapterOrStop();   // the chapter ran out: walk forward to one that speaks, else stop
    void notifyChanged();

    // Sleep timer plumbing.
    double nowSec() const;          // the monotonic clock the session is driven by
    bool   sleepStopAtBoundary(int nextChapter);   // ask the session; on yes, stop as the timer and say so
    void   syncSleepClock();        // the session's clock runs exactly while narration is speaking
    void   onSleepTick();           // the fade, while armed
    void   applySleepVolume(double gain);

    ReadAloudTarget* target_ = nullptr;
    QTextToSpeech*   tts_ = nullptr;
    QVector<ReadAloud::Utterance> utts_;
    QList<QVoice>    voices_;

    // Queue bookkeeping. `first_` is the utterance the engine's CURRENT queue starts at, and every
    // aboutToSynthesize is the next text of that queue starting, so first_ + spoken_ names the utterance.
    // (Qt's own id is not used: it is not reset by stop() - see onAboutToSynthesize.)
    int  first_   = 0;
    int  queued_  = 0;     // one past the last utterance handed to the engine
    int  spoken_  = 0;     // how many of the current queue have started (counted per aboutToSynthesize)
    int  current_ = -1;    // the utterance being spoken

    bool active_     = false;
    bool paused_     = false;   // see paused(): ours, because the engine's state enum does not say
    bool restarting_ = false;   // a stop() WE asked for: its Ready is not the end of the book
    bool pumping_    = false;   // inside pump(): an aboutToSynthesize emitted by enqueue() must not re-enter it
    double speed_    = 1.0;
    int    voiceIdx_ = 0;

    ReadAloud::SleepSession sleep_;
    QElapsedTimer mono_;
    QTimer* sleepTick_      = nullptr;
    double  utterStarted_   = 0.0;    // session-clock seconds at which the current utterance began
    double  baseVolume_     = -1.0;   // the engine's volume before any fade; <0 = no fade applied
    double  appliedGain_    = 1.0;
    int     shownMinutes_   = -2;     // the minutes-left the controls last showed, so a tick re-labels only on change
    // The last word the engine said while a timer is armed, and where it ended in its utterance: what the log
    // names when the timer fires, so a stop can be SEEN to have come after the utterance's last word.
    QString lastWord_;
    int     lastWordEnd_    = -1;

    static constexpr int kLookahead = 2;   // paragraphs kept in the engine's queue ahead of the spoken one
};
