// Typed, skippable ranges within a video — an episode's intro and its end credits — plus the three pure
// providers that produce them and the tracker that decides when one has been entered.
//
// Modeled on Jellyfin's Media Segments split: DETECTION (these providers) is separate from STORAGE
// (SegmentStore) and from ACTION (MainWindow's chip / auto-skip). A fourth provider — audio fingerprinting —
// would plug in here without touching either of the other two layers. Deliberately pure: no Qt GUI, no mpv,
// no file I/O, so probe_segments covers every rule in this file without a player or a fixture video.
#pragma once
#include <QString>
#include <QVector>
#include <optional>
#include <vector>

namespace MediaSegments
{
    enum class SegmentType { Intro, Credits, Recap, Commercial };

    struct Segment
    {
        double      start = 0.0;
        double      end   = 0.0;
        SegmentType type  = SegmentType::Intro;
    };

    // One mpv chapter. Declared HERE rather than in MpvWidget so core does not depend on the video layer —
    // and so probe_segments can test fromChapters() without linking libmpv.
    struct Chapter { double time = 0.0; QString title; };

    // The learned tier's identity. seriesKey is empty when nothing identifies a show (a movie, or a file
    // whose name carries no SxxExx), which is how callers know the learn tier is unavailable.
    struct Key { QString seriesKey; int season = 0; };

    // Ranges shorter than this are noise, not intros.
    constexpr double kMinSegmentS   = 5.0;
    // A range ending within this of the end of the file is the credits.
    constexpr double kCreditsTailS  = 60.0;
    // An intro starts before this, and runs no longer than kIntroMaxLenS.
    constexpr double kIntroWindowS  = 900.0;
    constexpr double kIntroMaxLenS  = 300.0;

    // Kodi .edl: "[start] [end] [action]", whitespace-separated, one range per line. Times are plain seconds
    // ("5.3"), "HH:MM:SS.sss", or "#<frame>". fps <= 0 means frames cannot be converted, so frame-form lines
    // are dropped and the rest of the file still applies. Actions: 0 cut, 1 mute, 2 scene marker, 3
    // commercial break — only 0 and 3 are skips. There is no comment syntax (and '#' cannot serve as one,
    // because it prefixes a frame number); any line that does not parse into three fields is dropped.
    QVector<Segment> parseEdl(const QString& text, double duration, double fps);

    // Chapters whose TITLE names a segment. A chapter runs to the next chapter's time, or to duration for
    // the last one (so a last chapter with duration <= 0 is dropped).
    QVector<Segment> fromChapters(const QVector<Chapter>& chapters, double duration);

    // Per-TYPE precedence: for each type independently, the first tier supplying that type wins. NOT
    // whole-list precedence, which would let an .edl carrying only a commercial break suppress a
    // chapter-derived Intro.
    QVector<Segment> resolve(const QVector<Segment>& edl,
                             const QVector<Segment>& chapters,
                             const QVector<Segment>& learned);

    // Series identity: the "tt…:S:E" stream id first, else the filename via LocalLibrary::parseFile.
    Key keyFor(const QString& imdbStreamId, const QString& localPath);

    // Stable tokens for SegmentStore's JSON. Unknown text maps back to Intro's absence (std::nullopt).
    QString                    typeToString(SegmentType t);
    std::optional<SegmentType> typeFromString(const QString& s);

    // Per-playback state: which segments have already been offered. Offers each at most once, until a
    // backward seek to before its start re-arms it, so scrubbing back and replaying re-offers the skip.
    class Tracker
    {
    public:
        void reset(QVector<Segment> segments);
        // The segment just entered, or nullopt. Call on every position tick.
        std::optional<Segment> onPosition(double t);
        bool empty() const { return segs_.isEmpty(); }

    private:
        QVector<Segment>  segs_;
        std::vector<bool> consumed_;
    };
}
