// ReadAloud — the PURE half of read-aloud (issue #145): how a chapter's text becomes utterances, what is
// stripped before an engine ever sees it, and how an utterance maps back onto the reading position.
//
// Everything here is QtCore-only and free of any engine, widget or store, so probe_readaloud can pin all of
// it on a headless runner with no audio device. That split is the point: the engine (QTextToSpeech) is an
// optional module that CI does not have, and the part that decides WHAT is spoken — the part that gets a
// division wrong, or reads "[12]" out loud, or loses your place — is exactly the part a probe can reach.
//
// THE POSITION MODEL. An utterance carries the character range [start, end) of the text it came from, in the
// SAME document-offset coordinates the reader already uses for everything else: BookPageWidget::topTextPosition,
// EbookView::gotoSpineOffset and ReaderAnchor's book anchor (spine + offset). So the spoken paragraph IS the
// reading position — there is no second bookmark system and no new mark kind. `text` is what the engine is
// asked to say (artifact-stripped, whitespace-collapsed); the RANGE stays in raw document coordinates so the
// highlight lands on the real words on the page.
#pragma once
#include <QString>
#include <QVector>
#include "ReaderAnchor.h"

namespace ReadAloud
{
    struct Utterance
    {
        int     start = 0;   // document offset of the first character of the source range
        int     end   = 0;   // one past the last — [start, end) is what the page highlights
        QString text;        // what the engine speaks: stripped + whitespace-collapsed (never empty)

        bool operator==(const Utterance& o) const
        { return start == o.start && end == o.end && text == o.text; }
    };

    // A paragraph longer than this many RAW characters is divided further, at sentence boundaries. Chosen for
    // the reason the issue gives: a whole-chapter utterance breaks pause/seek (you cannot stop mid-thought and
    // resume where you were), and a word-sized one sounds robotic. A paragraph is the natural unit of prose,
    // so it is the default; ~600 characters is roughly a long paragraph of a novel, past which an engine's
    // pause/resume granularity starts to feel coarse.
    inline constexpr int kLongParagraphChars = 600;

    // ---- Artifact stripping ---------------------------------------------------------------------------------
    // What a READER's eye skips but an engine would dutifully pronounce. Removed:
    //   * bracketed reference numbers — "[12]", "[3, 4]", "[7-9]" (digits, commas, spaces and dashes only, so
    //     "[sic]" and "[He laughed]" survive);
    //   * bracketed page artifacts — "[Page 12]", "[Pg 12]", "[p. 12]" (case-insensitive);
    //   * superscript digit footnote markers — U+00B9 U+00B2 U+00B3 and U+2070..U+2079;
    //   * footnote symbols — † ‡ § ‖ ¶;
    //   * every asterisk. As a footnote marker, a scene-break rule ("* * *") or Markdown emphasis it is never
    //     something a listener wants read, and no sentence of prose needs one;
    //   * invisible characters — soft hyphen U+00AD, ZWSP/ZWNJ/ZWJ U+200B..U+200D, BOM U+FEFF;
    //   * NBSP U+00A0 becomes a plain space, and runs of whitespace collapse to one; the result is trimmed.
    // KEPT, deliberately: every dialogue mark — " ' “ ” ‘ ’ — – — … ! ? , ; : . ( ) — because those are what
    // give spoken prose its rhythm, and stripping them is how read-aloud starts sounding like a list.
    QString stripArtifacts(const QString& raw);

    // Is this whole paragraph nothing but a page-number artifact — "12", "— 12 —", "[Pg 4]", "Page 199"? Such a
    // paragraph produces NO utterance at all (rather than an empty one), so read-aloud does not stop dead in
    // the middle of a chapter to announce a number. Deliberately narrow: only a paragraph that is ENTIRELY the
    // artifact, and only with arabic digits, so "see page 12" mid-prose and a roman-numeral chapter head ("II")
    // are both untouched.
    bool isPageArtifact(const QString& rawParagraph);

    // ---- Division -------------------------------------------------------------------------------------------
    // Divide ONE paragraph. Under kLongParagraphChars raw characters it is a single utterance spanning the whole
    // paragraph. Over it, whole sentences are accumulated greedily into chunks of at most `maxChars`; a single
    // sentence longer than that is broken at the last whitespace at or before the limit, so a break NEVER falls
    // inside a word. Returns empty for a paragraph that strips to nothing (blank lines, page artifacts).
    //
    // A '.' ends a sentence only when what precedes it is not a known abbreviation ("Mr.", "e.g.", "etc.") and
    // not a single-letter initial ("J. R. R. Tolkien"), and what follows it — past any closing quote or bracket —
    // is whitespace and then something that can start a sentence (an opening quote, a capital, or a digit). That
    // last clause is also what keeps dialogue whole: in `"Stop!" he said.` the '!' is followed by a lowercase
    // word, so it is not a boundary.
    QVector<Utterance> divideParagraph(const QString& paragraph, int paragraphStart,
                                       int maxChars = kLongParagraphChars);

    // The sentence boundaries divideParagraph splits on, exposed on their own so they can be pinned directly.
    // Returns the EXCLUSIVE end offset of every sentence, ascending, always closing with paragraph.size() - so
    // a paragraph the rules find no boundary in comes back as a single entry. This is the unit worth testing:
    // the grouping above merges whole sentences greedily, which means a spurious boundary usually leaves the
    // OUTPUT unchanged and only shows up on the day a paragraph happens to straddle the limit.
    QVector<int> sentenceEnds(const QString& paragraph);

    // Divide a whole chapter. `chapterPlainText` is the document's plain text — QTextDocument::toPlainText(),
    // whose offsets are exactly the offsets the reader positions with, because the block separator it writes is
    // ONE character wide just like the separator it replaced. Paragraphs are the '\n'-separated blocks.
    QVector<Utterance> plan(const QString& chapterPlainText, int maxChars = kLongParagraphChars);

    // ---- Position mapping -----------------------------------------------------------------------------------
    // The utterance a document offset belongs to: the one whose [start, end) contains it, else the first one
    // that begins after it (so an offset in the gap between paragraphs resumes at the NEXT paragraph, which is
    // what "read from here" means), else the last. -1 only when there are no utterances at all.
    int indexForOffset(const QVector<Utterance>& utterances, int offset);

    // The book anchor for an utterance — the SAME ReaderAnchor a bookmark and a stored position use, so the
    // reading position read-aloud leaves behind is indistinguishable from one a page turn left. A point anchor:
    // endOffset stays -1 (a spoken paragraph is a place you are, not a range you selected).
    ReaderAnchor anchorFor(int spine, const Utterance& u);

    // The inverse: which utterance a book anchor names. anchorFor/indexForAnchor round-trip for every utterance
    // in a plan — indexForAnchor(u, anchorFor(s, u[i])) == i — which is what makes "stop, and you are where the
    // narrator was" true rather than approximately true.
    int indexForAnchor(const QVector<Utterance>& utterances, const ReaderAnchor& a);

    // ---- Speed (issue #140's per-book memory, shared) --------------------------------------------------------
    // The speeds the reader's speed control steps through. Deliberately the SAME set the player's speed button
    // uses (MainWindow.cpp's kSpeedPresets), because it is the same stored preference: a narrated book and an
    // audiobook of the same book are one speed, per #140, and a value one surface can reach but the other
    // cannot would be a preference that changes when you switch how you are consuming it.
    QVector<double> speedSteps();

    // The next speed after `speed`, wrapping — the step a single "Speed" control takes. Matches to the nearest
    // step first, so a value that arrived from the player's own control (or from an older build) still advances
    // sensibly instead of snapping to the start.
    double nextSpeedStep(double speed);

    // A playback speed (1.0 == normal, the #140 scale) as QTextToSpeech's rate, which runs -1..1 around 0 for
    // the engine's own normal rate. Anchored at the three points that matter: 1.0 -> 0.0, 2.0 -> +1.0 and
    // 0.5 -> -1.0, linear on each side of 1.0 and clamped at both ends. A non-positive speed (a corrupt or
    // absent stored value) is normal rate, never a silent or a runaway one.
    double engineRateForSpeed(double speed);

    // ---- The reader chrome's control row ---------------------------------------------------------------------
    // How many controls the themed reader's settings row holds for a BOOK. Pure, and shared by the host (which
    // sets the nav zone count) so the feature-absent build is provably the row it always was: without the
    // TextToSpeech module there is no read-aloud control, the count is the historical 5, and the cursor cannot
    // stop on something that is not drawn. probe_readaloud pins both values.
    int bookSettingsRowCount(bool readAloudAvailable);
}
