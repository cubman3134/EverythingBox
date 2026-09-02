// RATE-LIMITING mpv's OWN LOG, HONESTLY (issue #231). Pure, header-only, QtCore-only, no clock of its own.
//
// THE PROBLEM. The whole point of #231 is that ffmpeg's per-frame complaints are the diagnosis:
//
//   [ffmpeg/video] hevc: Could not find ref with POC 47
//   [ffmpeg/video] hevc: concealing 3600 DC, 3600 AC, 3600 MV errors in P frame
//
// A damaged HEVC stream emits one of those PER FRAME — 24 a second, for as long as the user leaves it
// playing. Written straight through, an hour of a broken stream is ~86,000 lines into a file main.cpp caps
// at 1 MB and DELETES when it is exceeded, so the flood does not merely bury the evidence, it destroys the
// rest of the session's log along with it. Writing none of them is the state #231 exists to fix.
//
// THE SHAPE. A burst counter per MESSAGE SHAPE, with a summary line. The first few of a shape are written
// verbatim — those carry the real numbers and are what anybody reading the log actually needs — and the rest
// of the window is COUNTED and reported:
//
//   mpvlog [warn] ffmpeg/video: hevc: concealing 3600 DC, 3600 AC, 3600 MV errors in P frame
//   … (three more like it)
//   mpvlog … and 712 more in 30 s, last: [warn] ffmpeg/video: hevc: concealing 3584 DC, … in P frame
//
// Then the window reopens and the next 30 s does the same. So a stream that is broken for an hour costs
// ~10 lines a minute instead of ~1,400, no message is dropped without being counted, and the reader can
// still see the fault continuing and how bad it is.
//
// WHY THE KEY IS A SHAPE AND NOT THE TEXT. Those two lines differ in every occurrence — the POC and the
// error counts change per frame — so keying on the text would give every message its own bucket and throttle
// nothing at all. The key is the message with each run of digits replaced by '#', which is exactly the
// notion of "the same complaint again" a person reading the log has.
//
// WHY THE SUMMARY QUOTES THE **LAST** SUPPRESSED LINE AND NOT THE FIRST. The first is already in the file,
// four lines up. The last is the only thing in the window that is not otherwise recorded, and for a
// concealment burst it is the more useful of the two: it says what the damage looked like when the window
// closed, which is how you tell a fault that is clearing from one that is not.
//
// NO CLOCK. Every entry point takes `nowMs`. That is not purity for its own sake: it is what lets
// probe_mpvlog drive ten thousand messages across four windows in a few microseconds and assert the exact
// counts, rather than sleeping for two minutes and hoping.
#pragma once
#include <QHash>
#include <QLatin1String>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class MpvLogThrottle
{
public:
    // burst      — how many of one shape are written verbatim per window.
    // windowMs   — how long a window lasts before its summary is emitted and the burst allowance renews.
    // maxShapes  — the ceiling on distinct shapes tracked at once (see evict() for what happens above it).
    explicit MpvLogThrottle(int burst = 4, qint64 windowMs = 30000, int maxShapes = 64)
        : burst_(burst), windowMs_(windowMs), maxShapes_(maxShapes) {}

    // THE SHAPE OF A MESSAGE: every run of digits becomes a single '#'. Public because it is the whole basis
    // of the bucketing and a probe that could not state it could not pin it.
    static QString shapeOf(const QString& body)
    {
        QString k;
        k.reserve(body.size());
        bool inDigits = false;
        for (const QChar c : body)
        {
            if (c.isDigit()) { if (!inDigits) { k += QLatin1Char('#'); inDigits = true; } continue; }
            inDigits = false;
            k += c;
        }
        return k.left(240);   // bounded: a pathological single-line message must not become the memory leak
    }

    // Offer one message. Returns the lines that should be WRITTEN as a result, in order: any summaries whose
    // window closed on the way here, then the message itself when it is inside its burst allowance.
    QStringList admit(const QString& body, qint64 nowMs)
    {
        QStringList out = flush(nowMs);
        const QString key = shapeOf(body);
        auto it = buckets_.find(key);
        if (it == buckets_.end())
        {
            evict(nowMs, out);
            it = buckets_.insert(key, Bucket{ nowMs, 0, 0, nowMs, QString() });
        }
        Bucket& b = *it;
        b.lastSeen = nowMs;
        ++b.seen;
        if (b.seen <= burst_)
            out << body;
        else
        {
            ++b.suppressed;
            b.lastSuppressed = body;
        }
        return out;
    }

    // Summaries for every window that has CLOSED by `nowMs`. Called from admit(), and from a timer so a burst
    // that stops does not hold its own summary hostage until the next message arrives.
    QStringList flush(qint64 nowMs)
    {
        QStringList out;
        for (auto it = buckets_.begin(); it != buckets_.end(); )
        {
            Bucket& b = *it;
            if (nowMs - b.windowStart < windowMs_) { ++it; continue; }
            if (b.suppressed > 0) out << summary(b, nowMs);
            // A shape nobody has emitted for a whole window is forgotten, so that when it comes back — a
            // second damaged section an hour later — it gets its burst again instead of being silently
            // counted into a bucket left over from the first one. A shape still arriving keeps its bucket
            // and gets a fresh allowance.
            if (b.seen == 0) { it = buckets_.erase(it); continue; }
            b.windowStart = nowMs; b.seen = 0; b.suppressed = 0; b.lastSuppressed.clear();
            ++it;
        }
        return out;
    }

    // Every outstanding summary, whether or not its window has closed, and forget everything. For the end of
    // a file and for teardown: a burst that ran out the last twenty seconds of a stream still gets counted.
    QStringList drain(qint64 nowMs)
    {
        QStringList out;
        for (auto it = buckets_.begin(); it != buckets_.end(); ++it)
            if (it->suppressed > 0) out << summary(*it, nowMs);
        buckets_.clear();
        return out;
    }

    // Whether anything is being counted right now — the condition for keeping the flush timer running.
    bool pending() const
    {
        for (auto it = buckets_.begin(); it != buckets_.end(); ++it)
            if (it->suppressed > 0) return true;
        return false;
    }

    int trackedShapes() const { return int(buckets_.size()); }

private:
    struct Bucket
    {
        qint64  windowStart;
        int     seen;            // messages of this shape in the current window, admitted + suppressed
        int     suppressed;      // of those, the ones not written verbatim
        qint64  lastSeen;
        QString lastSuppressed;  // the most recent one that was counted rather than written
    };

    static QString summary(const Bucket& b, qint64 nowMs)
    {
        const qint64 secs = (nowMs - b.windowStart + 500) / 1000;
        return QStringLiteral("… and %1 more in %2 s, last: %3")
            .arg(b.suppressed).arg(secs).arg(b.lastSuppressed);
    }

    // At the ceiling, retire the shape nobody has seen for longest — reporting its count on the way out, so
    // even eviction does not drop a message silently. The ceiling exists because a source that puts a fresh
    // unique string in every message (a per-request id, a timestamp shapeOf cannot fold) would otherwise
    // grow this map without bound for the length of the session.
    void evict(qint64 nowMs, QStringList& out)
    {
        if (int(buckets_.size()) < maxShapes_) return;
        auto oldest = buckets_.begin();
        for (auto it = buckets_.begin(); it != buckets_.end(); ++it)
            if (it->lastSeen < oldest->lastSeen) oldest = it;
        if (oldest->suppressed > 0) out << summary(*oldest, nowMs);
        buckets_.erase(oldest);
    }

    QHash<QString, Bucket> buckets_;
    int    burst_;
    qint64 windowMs_;
    int    maxShapes_;
};
