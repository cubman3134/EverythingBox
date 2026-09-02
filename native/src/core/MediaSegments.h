// Typed, skippable ranges within a video — an episode's intro and its end credits — plus the three pure
// providers that produce them and the tracker that decides when one has been entered.
//
// Modeled on Jellyfin's Media Segments split: DETECTION (these providers) is separate from STORAGE
// (SegmentStore) and from ACTION (MainWindow's chip / auto-skip). A fourth provider — audio fingerprinting —
// would plug in here without touching either of the other two layers. Deliberately pure: no Qt GUI, no mpv,
// no file I/O, so probe_segments exercises it without a player or a fixture video: the three providers, the
// precedence rule, keyFor and the Tracker. The typeToString/typeFromString token mapping is covered in the
// same probe's SegmentStore section, where the tokens are actually used — both directions, distinctness, and
// the unknown-token nullopt.
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
    static_assert(kMinSegmentS > 0.0, "parseEdl/fromChapters rely on the minimum-length check to reject "
                                      "inverted and zero-length ranges; a zero minimum would let them through");
    // A range ending within this of the end of the file is the credits.
    constexpr double kCreditsTailS  = 60.0;
    // The earliest point, as a fraction of the file, at which a HAND-MARKED credits range may start. The
    // detector tiers get their lower bound for free (kCreditsTailS is measured from the end), but the learned
    // tier's "mark credits start here" takes whatever position the user is sitting at — and a mis-press stores
    // {now, duration}, which SegmentStore then inherits into every unmarked season of the show, so with
    // auto-skip on every episode jumps from that timestamp to the end. 0.6 is deliberately loose rather than
    // tight: real end credits start at ~90-97% of an episode, so 60% refuses only presses that cannot plausibly
    // be credits, while still accepting an unusually long tail (a post-credits scene, a "next week on…" trailer,
    // or a double-length episode whose credits run several minutes). Anything stricter would start refusing
    // legitimate marks, which is the worse failure: the user can see a mis-press, but not a rejected good one.
    constexpr double kCreditsEarliestFrac = 0.6;
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
    //
    // The learned tier is SPLIT, and the two halves sit at opposite ends of the order:
    //
    //  * exactLearned — a mark the user made against THIS season — is the HIGHEST tier. It is the only tier
    //    that is explicit, correctable and authored by the person watching: they saw what the detector offered,
    //    judged it wrong, and said so. Ranking it below the chapter detector made a hand-mark on a chaptered rip
    //    INERT while reporting success — put() stored it, the notice said "Intro remembered for season 1", and
    //    the chapter range went on winning, so nothing the user could see ever changed. An automatic detector
    //    must not outrank the correction aimed at it.
    //  * inheritedLearned — the SegmentStore nearest-season fallback, i.e. a mark another season made — stays
    //    the LOWEST tier. It is a guess that this season's opening matches a different season's, and a guess
    //    must never beat this file's own chapters: one season's hand-mark would otherwise override every other
    //    season's perfectly good chapter ranges, including seasons whose opening genuinely changed.
    //
    //  * server (issue #83) — what the MEDIA SERVER's own detector found for this item, read from
    //    Jellyfin's MediaSegments API. It sits between the .edl and the chapters, and the two neighbours
    //    are the argument: an .edl is a file a person hand-wrote for THIS rip and is authored the way an
    //    exact mark is, so it stays above; a chapter title is metadata a muxer wrote and is right about an
    //    intro only by coincidence, while the server has fingerprinted the whole series. (In practice the
    //    .edl and the server tier never collide at all — an .edl is a sidecar beside a local file and a
    //    server item has no local file — so the ordering between them is a statement of the rule rather
    //    than a decision anyone will feel. The one against CHAPTERS is real: a Jellyfin rip routinely has
    //    both.)
    QVector<Segment> resolve(const QVector<Segment>& exactLearned,
                             const QVector<Segment>& edl,
                             const QVector<Segment>& server,
                             const QVector<Segment>& chapters,
                             const QVector<Segment>& inheritedLearned);

    // The four-tier form, kept so that every caller and every probe written before #83 means exactly what
    // it meant: it is the five-tier rule with no server tier. NOT a defaulted parameter — a default would
    // let a caller that SHOULD be passing server segments silently pass none, which is the failure this
    // feature would show up as (the chip simply never appearing on server content).
    QVector<Segment> resolve(const QVector<Segment>& exactLearned,
                             const QVector<Segment>& edl,
                             const QVector<Segment>& chapters,
                             const QVector<Segment>& inheritedLearned);

    // Series identity: the "tt…:S:E" stream id first — matched on SHAPE, so a differently-shaped 3-part id
    // ("tmdb:tv:1396") is not mistaken for one — else the filename via LocalLibrary::parseFile.
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
        // Mark consumed every segment CONTAINING t, without offering any of them. Returns how many it took.
        //
        // For re-arming after a re-gather: reset() wipes consumed_, so a user who dismissed the intro chip and
        // then marked the credits — still sitting inside the intro — was instantly re-offered the intro they
        // had just dismissed (and, with auto-skip on, the freshly armed range fired on the very next tick).
        // Carrying the whole consumed_ set across a re-gather is not possible: the ranges themselves changed,
        // so the indices mean nothing. What CAN be carried is the only thing that matters — "do not offer me
        // something I am already inside" — which is exactly this. A range the user has not reached yet is
        // untouched and still fires normally.
        int consumeContaining(double t);

    private:
        QVector<Segment>  segs_;
        std::vector<bool> consumed_;
    };
}
