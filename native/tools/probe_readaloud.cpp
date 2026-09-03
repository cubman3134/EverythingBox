// Headless check of read-aloud's PURE half (issue #145): the utterance divider, the artifact stripper and the
// utterance <-> reading-position mapping in src/ebook/ReadAloud.{h,cpp}. QtCore-only and engine-free, which is
// the whole reason those three are pure: CI has neither the Qt TextToSpeech module nor an audio device, and the
// part of read-aloud that can be WRONG in a way a listener notices — a paragraph split mid-word, "[12]" read
// out loud, "Mr." heard as the end of a sentence, your place lost when you stop — is exactly this part.
//
// What is NOT here, and why: the engine. There is nothing to assert about QTextToSpeech that would not just be
// asserting Qt. The seam is what matters, so the divider's OUTPUT (offsets in the reader's own document
// coordinates, spoken text with the artifacts already gone) is pinned exhaustively instead.
//
// Every expectation below is hand-written from the design rather than recomputed by calling the code a second
// time. Sections:
//   1. stripping — bracketed refs / page brackets / superscripts / footnote symbols / asterisks / invisibles,
//      and the dialogue punctuation that must SURVIVE.
//   2. page artifacts — a paragraph that is only a page number produces no utterance; a roman-numeral heading
//      and "see page 12" inside prose do not.
//   3. division — a short paragraph is ONE utterance; a long one splits at sentence boundaries; a break is
//      never inside a word; "Mr." / "e.g." / initials / "3.14" / dialogue do not end a sentence.
//   4. position mapping — offsets are the reader's, ranges cover the source, and anchorFor/indexForAnchor
//      round-trip for every utterance in a plan.
//   5. the feature-absent build — the book's settings row is the historical 5 controls without the module.
//
// Prints READALOUD-OK on success; any failure prints READALOUD-FAIL <cond> (line) and exits non-zero.
#include "ReadAloud.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "READALOUD-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

#define EXPECT_STR(got, want) do { \
    const QString g_ = (got); \
    const QString w_ = (want); \
    if (g_ != w_) { \
        std::fprintf(stderr, "READALOUD-FAIL %s -> got '%s' want '%s' (line %d)\n", \
                     #got, g_.toUtf8().constData(), w_.toUtf8().constData(), __LINE__); \
        ++failures; \
    } \
} while (0)

using U = ReadAloud::Utterance;

// ---- 1. Artifact stripping ---------------------------------------------------------------------------------

static void testStripping()
{
    // Bracketed reference numbers, in the shapes a scanned book actually carries.
    EXPECT_STR(ReadAloud::stripArtifacts(QStringLiteral("The war ended[12] in autumn.")),
               QStringLiteral("The war ended in autumn."));
    EXPECT_STR(ReadAloud::stripArtifacts(QStringLiteral("Several sources[3, 4] agree.")),
               QStringLiteral("Several sources agree."));
    EXPECT_STR(ReadAloud::stripArtifacts(QStringLiteral("As shown[7-9] earlier.")),
               QStringLiteral("As shown earlier."));

    // Bracketed PAGE artifacts, case-insensitive, all three spellings.
    EXPECT_STR(ReadAloud::stripArtifacts(QStringLiteral("He left[Page 12] at dawn.")),
               QStringLiteral("He left at dawn."));
    EXPECT_STR(ReadAloud::stripArtifacts(QStringLiteral("He left[pg 12] at dawn.")),
               QStringLiteral("He left at dawn."));
    EXPECT_STR(ReadAloud::stripArtifacts(QStringLiteral("He left[p. 12] at dawn.")),
               QStringLiteral("He left at dawn."));

    // An EDITORIAL bracket is prose and must survive — this is the line between the two.
    EXPECT_STR(ReadAloud::stripArtifacts(QStringLiteral("He said it [sic] plainly.")),
               QStringLiteral("He said it [sic] plainly."));

    // Superscript digit markers.
    EXPECT_STR(ReadAloud::stripArtifacts(QString::fromUtf8("A footnote\xC2\xB9 here.")),
               QStringLiteral("A footnote here."));
    EXPECT_STR(ReadAloud::stripArtifacts(QString::fromUtf8("And another\xE2\x81\xB5 one.")),
               QStringLiteral("And another one."));

    // Footnote symbols and the asterisk.
    EXPECT_STR(ReadAloud::stripArtifacts(QString::fromUtf8("Marked\xE2\x80\xA0 and marked\xE2\x80\xA1 again.")),
               QStringLiteral("Marked and marked again."));
    EXPECT_STR(ReadAloud::stripArtifacts(QString::fromUtf8("See \xC2\xA7 four.")),
               QStringLiteral("See four."));
    EXPECT_STR(ReadAloud::stripArtifacts(QStringLiteral("A word* with a marker.")),
               QStringLiteral("A word with a marker."));
    EXPECT_STR(ReadAloud::stripArtifacts(QStringLiteral("* * *")), QString());

    // Invisibles: soft hyphen, zero-width space, BOM. NBSP becomes a real space and collapses.
    EXPECT_STR(ReadAloud::stripArtifacts(QString::fromUtf8("hy\xC2\xADphen")), QStringLiteral("hyphen"));
    EXPECT_STR(ReadAloud::stripArtifacts(QString::fromUtf8("zero\xE2\x80\x8Bwidth")), QStringLiteral("zerowidth"));
    EXPECT_STR(ReadAloud::stripArtifacts(QString::fromUtf8("two\xC2\xA0 spaces")), QStringLiteral("two spaces"));
    EXPECT_STR(ReadAloud::stripArtifacts(QStringLiteral("  ragged\n\t  lines  ")), QStringLiteral("ragged lines"));

    // DIALOGUE PUNCTUATION SURVIVES — the whole point of stripping being narrow. Curly quotes, an em dash, an
    // ellipsis, a question mark and an exclamation all pass through untouched.
    const QString dialogue = QString::fromUtf8(
        "\xE2\x80\x9CStop!\xE2\x80\x9D he cried \xE2\x80\x94 \xE2\x80\x98why?\xE2\x80\x99 \xE2\x80\xA6 nothing.");
    EXPECT_STR(ReadAloud::stripArtifacts(dialogue), dialogue);
}

// ---- 2. Page artifacts -------------------------------------------------------------------------------------

static void testPageArtifacts()
{
    CHECK(ReadAloud::isPageArtifact(QStringLiteral("12")));
    CHECK(ReadAloud::isPageArtifact(QStringLiteral("  199  ")));
    CHECK(ReadAloud::isPageArtifact(QString::fromUtf8("\xE2\x80\x94 12 \xE2\x80\x94")));
    CHECK(ReadAloud::isPageArtifact(QStringLiteral("- 4 -")));
    CHECK(ReadAloud::isPageArtifact(QStringLiteral("(23)")));
    CHECK(ReadAloud::isPageArtifact(QStringLiteral("Page 199")));
    CHECK(ReadAloud::isPageArtifact(QStringLiteral("[Pg 4]")));

    // Not artifacts: a roman-numeral chapter head, a numbered chapter title, prose that mentions a page, and a
    // blank line (which is nothing, not a number).
    CHECK(!ReadAloud::isPageArtifact(QStringLiteral("II")));
    CHECK(!ReadAloud::isPageArtifact(QStringLiteral("Chapter 12")));
    CHECK(!ReadAloud::isPageArtifact(QStringLiteral("see page 12 for the map")));
    CHECK(!ReadAloud::isPageArtifact(QStringLiteral("   ")));

    // And the consequence: such a paragraph yields NO utterance, so narration does not stop to say "twelve".
    CHECK(ReadAloud::divideParagraph(QStringLiteral("12"), 0).isEmpty());
    CHECK(ReadAloud::divideParagraph(QStringLiteral("II"), 0).size() == 1);
}

// ---- 3. Division -------------------------------------------------------------------------------------------

static void testShortParagraphIsOneUtterance()
{
    const QString p = QStringLiteral("A short paragraph. It has two sentences, and it is well under the limit.");
    const QVector<U> u = ReadAloud::divideParagraph(p, 100);
    CHECK(u.size() == 1);
    if (u.size() == 1)
    {
        CHECK(u[0].start == 100);
        CHECK(u[0].end == 100 + p.size());     // the range covers the WHOLE paragraph, offsets are the caller's
        EXPECT_STR(u[0].text, p);
    }
}

static void testLongParagraphSplitsAtSentences()
{
    // Nine sentences of ~90 characters each. With a 200-character limit the divider must group WHOLE sentences,
    // so every piece ends on a terminator and no piece exceeds the limit by more than one sentence's tail.
    QString p;
    for (int i = 1; i <= 9; ++i)
        p += QStringLiteral("This is sentence number %1 and it runs on for a while so the paragraph gets long. ")
                 .arg(i);
    p = p.trimmed();
    CHECK(p.size() > 600);

    const QVector<U> u = ReadAloud::divideParagraph(p, 0, 200);
    CHECK(u.size() >= 4);
    for (const U& x : u)
    {
        CHECK(!x.text.isEmpty());
        CHECK(x.end > x.start);
        // Every piece ends on a sentence terminator: the split fell on a boundary, not an arbitrary offset.
        CHECK(x.text.endsWith(QLatin1Char('.')));
        // NEVER MID-WORD, in both directions: the character before a piece is whitespace (or the start), and
        // the character after it is whitespace (or the end).
        CHECK(x.start == 0 || p.at(x.start - 1).isSpace());
        CHECK(x.end == p.size() || p.at(x.end).isSpace());
    }
    // The pieces are in order and cover the paragraph without overlapping.
    for (int i = 1; i < u.size(); ++i) CHECK(u[i].start >= u[i - 1].end);
    CHECK(u.first().start == 0);
    CHECK(u.last().end == p.size());
}

static void testOverlongSentenceBreaksOnWhitespace()
{
    // One sentence, no interior terminator, far longer than the limit: the only way to divide it is inside the
    // sentence — and it still may not land inside a word.
    QString p;
    while (p.size() < 900) p += QStringLiteral("wordy ");
    p = p.trimmed() + QLatin1Char('.');

    const QVector<U> u = ReadAloud::divideParagraph(p, 0, 200);
    CHECK(u.size() >= 4);
    for (const U& x : u)
    {
        CHECK(x.end - x.start <= 200);
        CHECK(x.start == 0 || p.at(x.start - 1).isSpace());
        CHECK(x.end == p.size() || p.at(x.end).isSpace());
        CHECK(!x.text.startsWith(QLatin1Char(' ')) && !x.text.endsWith(QLatin1Char(' ')));
    }
    CHECK(u.last().end == p.size());
}

static void testAbbreviationsAndDialogueDoNotEndSentences()
{
    // The boundaries themselves, not the grouping over them. Asserting through divideParagraph would hide
    // these: whole sentences are accumulated greedily, so a spurious boundary usually produces the SAME output
    // and only surfaces the day a paragraph happens to straddle the chunk limit.
    struct Case { const char* text; int wantEnds; int firstEnd; };
    const Case cases[] = {
        // Exactly ONE entry (the paragraph end) means the rules found no interior boundary at all.
        { "Mr. Wickham arrived at noon and Mrs. Bennet was delighted.", 1, -1 },   // abbreviation
        { "The book was written by J. R. R. Tolkien over many years.",  1, -1 },   // single-letter initials
        { "Bring rope, e.g. the long one, and tinder, etc. and hurry.", 1, -1 },   // "e.g." / "etc."
        { "The value of pi is roughly 3.14 which was quite enough.",    1, -1 },   // a decimal
        { "\"Stop that at once!\" he said, loudly enough to be heard.",  1, -1 },   // dialogue: lowercase follows
        // And the converse, so none of the above is just "never split": a real boundary IS one, and the
        // closing quote goes with the sentence it ended.
        { "He walked home. She stayed behind.", 2, 15 },
        { "He said \"Go.\" She left at once.",    2, 13 },
        { "He was born in 1892. Then he left.", 2, 20 },
    };

    for (const Case& c : cases)
    {
        const QString p = QString::fromUtf8(c.text);
        const QVector<int> ends = ReadAloud::sentenceEnds(p);
        bool ok = ends.size() == c.wantEnds && !ends.isEmpty() && ends.constLast() == p.size();
        if (ok && c.firstEnd >= 0) ok = ends[0] == c.firstEnd;
        if (!ok)
        {
            QString got;
            for (int e : ends) got += QString::number(e) + QLatin1Char(' ');
            const QByteArray gotUtf8 = got.trimmed().toUtf8();
            std::fprintf(stderr, "READALOUD-FAIL sentenceEnds('%s') -> [%s] want %d end(s), first %d (line %d)\n",
                         c.text, gotUtf8.constData(), c.wantEnds, c.firstEnd, __LINE__);
            ++failures;
        }
    }

    // The grouping over those boundaries: two sentences, a limit that fits one, and the pieces are the two
    // sentences exactly - trimmed, whole, and in order.
    const QString two = QStringLiteral("He walked home in the rain. She stayed behind and locked the door.");
    const QVector<U> u = ReadAloud::divideParagraph(two, 0, 40);
    CHECK(u.size() == 2);
    if (u.size() == 2)
    {
        EXPECT_STR(u[0].text, QStringLiteral("He walked home in the rain."));
        EXPECT_STR(u[1].text, QStringLiteral("She stayed behind and locked the door."));
    }
}

static void testPlanAcrossParagraphs()
{
    // A chapter as QTextDocument::toPlainText() hands it over: '\n'-separated blocks, one character per
    // separator, so an offset here is an offset the reader can position to.
    const QString chapter = QStringLiteral("Chapter One\n")          //  0..10, sep at 11
                          + QStringLiteral("\n")                      // 12: a blank line
                          + QStringLiteral("The first paragraph.\n")  // 13..32, sep at 33
                          + QStringLiteral("14\n")                    // 34..35: a page artifact
                          + QStringLiteral("The last paragraph.");    // 37..55

    const QVector<U> u = ReadAloud::plan(chapter);
    CHECK(u.size() == 3);                                   // blank line and page number produce nothing
    if (u.size() == 3)
    {
        EXPECT_STR(u[0].text, QStringLiteral("Chapter One"));
        CHECK(u[0].start == 0);
        EXPECT_STR(u[1].text, QStringLiteral("The first paragraph."));
        CHECK(u[1].start == 13);
        CHECK(chapter.mid(u[1].start, u[1].end - u[1].start) == QStringLiteral("The first paragraph."));
        EXPECT_STR(u[2].text, QStringLiteral("The last paragraph."));
        CHECK(chapter.mid(u[2].start, u[2].end - u[2].start) == QStringLiteral("The last paragraph."));
    }
}

// ---- 4. Position mapping -----------------------------------------------------------------------------------

static void testPositionMapping()
{
    const QString chapter = QStringLiteral("Alpha paragraph.\nBeta paragraph.\nGamma paragraph.");
    const QVector<U> u = ReadAloud::plan(chapter);
    CHECK(u.size() == 3);
    if (u.size() != 3) return;

    // An offset INSIDE an utterance names that utterance.
    CHECK(ReadAloud::indexForOffset(u, u[0].start) == 0);
    CHECK(ReadAloud::indexForOffset(u, u[1].start + 3) == 1);
    CHECK(ReadAloud::indexForOffset(u, u[2].end - 1) == 2);

    // An offset in the GAP between paragraphs resumes at the NEXT one — "read from here" reads forward.
    CHECK(ReadAloud::indexForOffset(u, u[0].end) == 1);

    // Past the end clamps to the last; before the start clamps to the first; empty is -1.
    CHECK(ReadAloud::indexForOffset(u, chapter.size() + 500) == 2);
    CHECK(ReadAloud::indexForOffset(u, -5) == 0);
    CHECK(ReadAloud::indexForOffset(QVector<U>(), 0) == -1);

    // ROUND TRIP through the SAME ReaderAnchor a bookmark and a stored position use: an utterance -> its anchor
    // -> back to the same utterance, for every utterance in the plan. This is what makes "stop read-aloud and
    // you are exactly where the narrator was" a property rather than a hope.
    for (int i = 0; i < u.size(); ++i)
    {
        const ReaderAnchor a = ReadAloud::anchorFor(7, u[i]);
        CHECK(a.kind == ReaderAnchor::Book);
        CHECK(a.spine == 7);
        CHECK(a.offset == u[i].start);
        CHECK(a.endOffset == -1);                       // a point anchor, not a highlight range
        CHECK(ReadAloud::indexForAnchor(u, a) == i);
        // And it survives the anchor's own JSON round trip, so a position stored on one device and read on
        // another names the same paragraph.
        CHECK(ReadAloud::indexForAnchor(u, ReaderAnchor::fromJson(a.toJson())) == i);
    }

    // A pdf/comic anchor is not a book position and must not silently resolve to one.
    ReaderAnchor pdf;
    pdf.kind = ReaderAnchor::Pdf;
    pdf.page = 3;
    CHECK(ReadAloud::indexForAnchor(u, pdf) == -1);
}

// ---- 5. The feature-absent build ---------------------------------------------------------------------------

static void testFeatureAbsentRow()
{
    // Without the TextToSpeech module the reader's control row is the five it has always had — Exit, font -,
    // font +, theme, typeface — so nothing is drawn that cannot act and the nav cursor cannot stop on a control
    // that is not there. With the module it gains exactly four: speak/stop, pause/resume, speed, voice.
    CHECK(ReadAloud::bookSettingsRowCount(false) == 5);
    CHECK(ReadAloud::bookSettingsRowCount(true) == 9);
    CHECK(ReadAloud::bookSettingsRowCount(true) - ReadAloud::bookSettingsRowCount(false) == 4);
}

// ---- 6. Speed (the shared #140 preference) ------------------------------------------------------------------

static void testSpeed()
{
    // The SAME seven presets the player's speed button steps through — the point of #140's per-book memory is
    // that a narrated book and an audiobook are one preference, and a value one surface cannot reach would
    // break that. Hand-written, not read back from the player.
    const QVector<double> steps = ReadAloud::speedSteps();
    CHECK(steps.size() == 7);
    if (steps.size() == 7)
    {
        const double want[7] = { 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0 };
        for (int i = 0; i < 7; ++i) CHECK(qFuzzyCompare(steps[i], want[i]));
    }

    // Stepping advances and wraps, and an off-grid value (from the player's control, or an older build) snaps
    // to its neighbour's successor rather than to the start of the list.
    CHECK(qFuzzyCompare(ReadAloud::nextSpeedStep(1.0), 1.25));
    CHECK(qFuzzyCompare(ReadAloud::nextSpeedStep(2.0), 0.5));
    CHECK(qFuzzyCompare(ReadAloud::nextSpeedStep(1.3), 1.5));

    // Speed -> the engine's -1..1 rate, anchored at the three points that matter and clamped past them. A
    // corrupt or absent speed is NORMAL rate: the failure mode of getting this wrong is a book that will not
    // speak, or one that gabbles, and neither says why.
    CHECK(qFuzzyCompare(ReadAloud::engineRateForSpeed(1.0) + 1.0, 1.0));   // == 0.0
    CHECK(qFuzzyCompare(ReadAloud::engineRateForSpeed(2.0), 1.0));
    CHECK(qFuzzyCompare(ReadAloud::engineRateForSpeed(0.5), -1.0));
    CHECK(qFuzzyCompare(ReadAloud::engineRateForSpeed(1.5), 0.5));
    CHECK(qFuzzyCompare(ReadAloud::engineRateForSpeed(0.75), -0.5));
    CHECK(qFuzzyCompare(ReadAloud::engineRateForSpeed(9.0), 1.0));         // clamped
    CHECK(qFuzzyCompare(ReadAloud::engineRateForSpeed(0.1), -1.0));        // clamped
    CHECK(qFuzzyCompare(ReadAloud::engineRateForSpeed(0.0) + 1.0, 1.0));   // == 0.0
    CHECK(qFuzzyCompare(ReadAloud::engineRateForSpeed(-3.0) + 1.0, 1.0));  // == 0.0
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testStripping();
    testPageArtifacts();
    testShortParagraphIsOneUtterance();
    testLongParagraphSplitsAtSentences();
    testOverlongSentenceBreaksOnWhitespace();
    testAbbreviationsAndDialogueDoNotEndSentences();
    testPlanAcrossParagraphs();
    testPositionMapping();
    testSpeed();
    testFeatureAbsentRow();
    if (failures == 0) std::printf("READALOUD-OK\n");
    return failures == 0 ? 0 : 1;
}
