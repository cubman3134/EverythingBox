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

#include "ReadAloud.h"

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
    bool paused() const;
    // The utterance being spoken, or -1. Only meaningful while active.
    int  currentUtterance() const { return current_; }

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

signals:
    void changed();        // active/paused/paragraph/speed/voice moved

private slots:
    void onAboutToSynthesize(qsizetype id);
    void onStateChanged(QTextToSpeech::State s);

private:
    void loadVoices();             // gather the offered voices and restore the stored pick
    void applySpeed();
    void applyVoice();
    void pump();                   // top the engine's queue back up to the look-ahead
    void speakFrom(int index);     // restart the queue at an utterance
    bool planCurrentChapter();     // (re)divide the chapter the reader is on; false when it has nothing to say
    void advanceChapterOrStop();   // the chapter ran out: walk forward to one that speaks, else stop
    void notifyChanged();

    ReadAloudTarget* target_ = nullptr;
    QTextToSpeech*   tts_ = nullptr;
    QVector<ReadAloud::Utterance> utts_;
    QList<QVoice>    voices_;

    // Queue bookkeeping. `first_` is the utterance the engine's CURRENT queue starts at, so the id Qt reports
    // in aboutToSynthesize (an index into that queue, reset by stop()) maps to an utterance by simple addition.
    int  first_   = 0;
    int  queued_  = 0;     // one past the last utterance handed to the engine
    int  spoken_  = 0;     // how many of the current queue have started
    int  current_ = -1;    // the utterance being spoken

    bool active_     = false;
    bool restarting_ = false;   // a stop() WE asked for: its Ready is not the end of the book
    double speed_    = 1.0;
    int    voiceIdx_ = 0;

    static constexpr int kLookahead = 2;   // paragraphs kept in the engine's queue ahead of the spoken one
};
