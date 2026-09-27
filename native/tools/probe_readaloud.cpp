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
//   6. speed — the shared #140 presets, stepping, and the speed -> engine-rate map.
//   8. the sleep timer on narration (#145, over #140's SleepTimer) - a FAKE clock throughout: minutes are minutes
//      of narration (a pause does not count), End of chapter fires at the spine boundary and not before, a
//      minute expiry mid-utterance waits for the utterance to end, the fade schedule (down to a floor, never
//      silence, while the last utterance finishes), both lifetime edges disarm, and Off does nothing.
//   7. voice choice (#137 / #283) — ReadAloud::chooseVoices over an INJECTED voice list (the runner has no
//      voices): a book's declared language offers only its voices, regional variants included; no matching
//      voice, or no preference, offers ALL voices (today's rule, pinned as-is); the stored pick is restored by
//      name through a reordering; a stored name that is gone, or not offered, falls back to index 0.
//
// Prints READALOUD-OK on success; any failure prints READALOUD-FAIL <cond> (line) and exits non-zero.
#include "ReadAloud.h"
#include "ReadAloudSleep.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QLocale>
#include <QString>
#include <QStringList>
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
    // that is not there. With the module it gains exactly five: speak/stop, pause/resume, speed, voice and the
    // sleep timer (#145).
    CHECK(ReadAloud::bookSettingsRowCount(false) == 5);
    CHECK(ReadAloud::bookSettingsRowCount(true) == 10);
    CHECK(ReadAloud::bookSettingsRowCount(true) - ReadAloud::bookSettingsRowCount(false) == 5);
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

// ---- 7. Voice choice (#137 / #283) --------------------------------------------------------------------------

// The offered voices by NAME, comma-joined in offered order, so a failure reads as the two lists side by side.
static QString offeredNames(const QVector<ReadAloud::VoiceOption>& installed, const ReadAloud::VoiceChoice& c)
{
    QStringList names;
    for (int i : c.offered)
        names << ((i >= 0 && i < installed.size()) ? installed[i].name : QStringLiteral("<bad index>"));
    return names.join(QLatin1Char(','));
}

static ReadAloud::VoiceOption voice(const char* name, const char* locale)
{
    return ReadAloud::VoiceOption{ QString::fromLatin1(name), QLocale(QString::fromLatin1(locale)) };
}

static void testVoiceChoice()
{
    // An engine's list, in the engine's order: two English, two French in DIFFERENT regions, one German.
    const QVector<ReadAloud::VoiceOption> installed = {
        voice("David",    "en_US"),
        voice("Hortense", "fr_FR"),
        voice("Zira",     "en_US"),
        voice("Sylvie",   "fr_CA"),
        voice("Hedda",    "de_DE"),
    };
    const QString all = QStringLiteral("David,Hortense,Zira,Sylvie,Hedda");

    // A "fr" book offers ONLY the French voices, in the engine's order - and the fr_CA voice is one of them:
    // the match is on language, not region. Nothing stored, so the first offered voice is the pick.
    ReadAloud::VoiceChoice c = ReadAloud::chooseVoices(installed, QStringLiteral("fr"), QString());
    EXPECT_STR(offeredNames(installed, c), QStringLiteral("Hortense,Sylvie"));
    CHECK(c.selected == 0);

    // A region-qualified preference is not narrowed by its region either, in both spellings a book carries:
    // dc:language is BCP 47 ("fr-CA"), a system locale name is "fr_CA".
    c = ReadAloud::chooseVoices(installed, QStringLiteral("fr-CA"), QString());
    EXPECT_STR(offeredNames(installed, c), QStringLiteral("Hortense,Sylvie"));
    c = ReadAloud::chooseVoices(installed, QStringLiteral("fr_FR"), QString());
    EXPECT_STR(offeredNames(installed, c), QStringLiteral("Hortense,Sylvie"));

    // A French book with NO French voice installed offers EVERY voice, in the engine's order. This is the rule
    // the code has today and it is pinned as-is: it is NOT "the system locale's voices" (#283 records the
    // difference for the owner). An English-only machine: the French book is offered the English voices, as
    // the whole list, not as a narrowing.
    const QVector<ReadAloud::VoiceOption> noFrench = {
        voice("David", "en_US"), voice("Hedda", "de_DE"), voice("Zira", "en_GB"),
    };
    c = ReadAloud::chooseVoices(noFrench, QStringLiteral("fr"), QString());
    EXPECT_STR(offeredNames(noFrench, c), QStringLiteral("David,Hedda,Zira"));
    CHECK(c.selected == 0);
    // ...and the same for a language nothing on the machine speaks at all.
    c = ReadAloud::chooseVoices(installed, QStringLiteral("ja"), QString());
    EXPECT_STR(offeredNames(installed, c), all);
    CHECK(c.selected == 0);

    // An EMPTY preference offers every voice. (EbookView::bookLanguage already turns a book that declares
    // nothing into the system locale's name, so this is the no-target path - and it must stay "everything".)
    c = ReadAloud::chooseVoices(installed, QString(), QString());
    EXPECT_STR(offeredNames(installed, c), all);
    CHECK(c.selected == 0);

    // The stored pick is restored BY NAME, as an index into the OFFERED list...
    c = ReadAloud::chooseVoices(installed, QStringLiteral("fr"), QStringLiteral("Sylvie"));
    EXPECT_STR(offeredNames(installed, c), QStringLiteral("Hortense,Sylvie"));
    CHECK(c.selected == 1);
    // ...and it survives the engine listing its voices in a different order (a voice installed, an update):
    // the same name is found at its new place, not the old index.
    const QVector<ReadAloud::VoiceOption> reordered = {
        voice("Hedda",    "de_DE"),
        voice("Sylvie",   "fr_CA"),
        voice("Zira",     "en_US"),
        voice("Hortense", "fr_FR"),
        voice("David",    "en_US"),
    };
    c = ReadAloud::chooseVoices(reordered, QStringLiteral("fr"), QStringLiteral("Sylvie"));
    EXPECT_STR(offeredNames(reordered, c), QStringLiteral("Sylvie,Hortense"));
    CHECK(c.selected == 0);
    c = ReadAloud::chooseVoices(reordered, QStringLiteral("fr"), QStringLiteral("Hortense"));
    CHECK(c.selected == 1);
    // In the all-voices fallback too.
    c = ReadAloud::chooseVoices(installed, QStringLiteral("ja"), QStringLiteral("Hedda"));
    CHECK(c.selected == 4);

    // A stored name that is no longer installed falls back to the first offered voice.
    c = ReadAloud::chooseVoices(installed, QStringLiteral("fr"), QStringLiteral("Amelie"));
    EXPECT_STR(offeredNames(installed, c), QStringLiteral("Hortense,Sylvie"));
    CHECK(c.selected == 0);
    // So does one that IS installed but is not offered for this book: an English pick is not carried into a
    // French book's French-only list.
    c = ReadAloud::chooseVoices(installed, QStringLiteral("fr"), QStringLiteral("Zira"));
    EXPECT_STR(offeredNames(installed, c), QStringLiteral("Hortense,Sylvie"));
    CHECK(c.selected == 0);

    // No voices at all: nothing offered, and the pick is still 0 (the controller's applyVoice bounds-checks it).
    c = ReadAloud::chooseVoices(QVector<ReadAloud::VoiceOption>(), QStringLiteral("fr"), QStringLiteral("Sylvie"));
    CHECK(c.offered.isEmpty());
    CHECK(c.selected == 0);
}

// ---- 8. The sleep timer on narration (#145) ------------------------------------------------------------------
// Every time below is a literal on a FAKE clock: seconds on whatever monotonic clock the controller keeps.

static bool near(double a, double b) { return a - b < 1e-9 && b - a < 1e-9; }

static SleepTimer::Timer minutesTimer(double m)
{
    SleepTimer::Timer t;
    t.mode = SleepTimer::Mode::Minutes;
    t.minutes = m;
    return t;
}
static SleepTimer::Timer chapterTimer()
{
    SleepTimer::Timer t;
    t.mode = SleepTimer::Mode::EndOfChapter;
    return t;
}
static SleepTimer::Timer offTimer() { return SleepTimer::Timer(); }

static void testSleepMinutesExcludePause()
{
    ReadAloud::SleepSession s;
    CHECK(s.arm(minutesTimer(1), 100.0, 3, /*paused*/ false));
    CHECK(s.armed());
    CHECK(s.mode() == SleepTimer::Mode::Minutes);

    // Twenty seconds in, narration is paused for twenty seconds of wall time.
    s.pause(120.0);
    s.pause(125.0);                       // re-asserting a pause changes nothing
    CHECK(near(s.narrated(130.0), 20.0)); // the clock stood still while paused
    s.resume(140.0);
    s.resume(141.0);                      // nor does re-asserting a resume
    CHECK(near(s.narrated(150.0), 30.0));

    // Sixty wall-seconds after arming it has narrated only forty: not due, and no boundary stops it.
    CHECK(!s.due(160.0));
    CHECK(!s.stopAtBoundary(160.0, 3));
    // One narrated minute is wall-time 180 (60 + the 20 paused). Just before: not yet.
    CHECK(!s.due(179.9));
    CHECK(!s.stopAtBoundary(179.9, 3));
    CHECK(s.due(180.0));
    CHECK(s.stopAtBoundary(180.0, 3));    // a boundary in the SAME chapter stops it once the minutes are up
    CHECK(near(s.secondsLeft(170.0, -1.0), 10.0));
    CHECK(near(s.secondsLeft(200.0, -1.0), 0.0));

    // Armed while paused: the clock starts stopped, and only narration moves it.
    ReadAloud::SleepSession p;
    CHECK(p.arm(minutesTimer(1), 0.0, 0, /*paused*/ true));
    CHECK(near(p.narrated(1000.0), 0.0));
    CHECK(!p.due(1000.0));
    p.resume(1000.0);
    CHECK(!p.due(1059.0));
    CHECK(p.due(1060.0));
}

static void testSleepEndOfChapterAtTheBoundary()
{
    ReadAloud::SleepSession s;
    CHECK(s.arm(chapterTimer(), 0.0, 4, false));
    CHECK(s.mode() == SleepTimer::Mode::EndOfChapter);
    // Every boundary INSIDE chapter 4 carries on, however long the chapter runs.
    CHECK(!s.stopAtBoundary(0.0, 4));
    CHECK(!s.stopAtBoundary(30.0, 4));
    CHECK(!s.stopAtBoundary(36000.0, 4));
    // End of chapter is decided at the boundary alone; time never makes it due.
    CHECK(!s.due(36000.0));
    // Crossing into the next spine item is where it stops - at the start of the next chapter.
    CHECK(s.stopAtBoundary(40.0, 5));
    // A pause mid-chapter does not change the rule.
    s.pause(50.0);
    s.resume(60.0);
    CHECK(!s.stopAtBoundary(70.0, 4));
    CHECK(s.stopAtBoundary(70.0, 5));
}

// The controller asks only at utterance boundaries. Given the moments the utterances END, the first boundary at
// which it stops - or -1 when none does.
static double firstStop(const ReadAloud::SleepSession& s, const QVector<double>& ends, int chapter)
{
    for (double t : ends)
        if (s.stopAtBoundary(t, chapter)) return t;
    return -1.0;
}

static void testSleepExpiryWaitsForTheUtterance()
{
    ReadAloud::SleepSession s;
    CHECK(s.arm(minutesTimer(1), 0.0, 2, false));
    // Utterances end at 25, 50, 70 and 95: the one being spoken at the 60 s expiry runs 50..70.
    const QVector<double> ends = { 25.0, 50.0, 70.0, 95.0 };
    CHECK(s.due(60.0));                       // the minutes are up mid-utterance...
    CHECK(near(firstStop(s, ends, 2), 70.0)); // ...and it stops when THAT utterance ends: not before, not later
    // Still audible while it finishes: the fade bottoms out at the floor, not at silence.
    CHECK(s.gain(65.0, -1.0) > 0.0);
    CHECK(near(s.gain(65.0, -1.0), ReadAloud::kSleepFadeFloor));
}

static void testSleepFadeSchedule()
{
    CHECK(near(ReadAloud::kSleepFadeWindowSec, 10.0));
    CHECK(near(ReadAloud::kSleepFadeFloor, 0.25));

    ReadAloud::SleepSession s;
    CHECK(s.arm(minutesTimer(1), 0.0, 0, false));
    CHECK(near(s.gain(0.0, -1.0), 1.0));
    CHECK(near(s.gain(49.9, -1.0), 1.0));    // more than the window left: full volume
    CHECK(near(s.gain(50.0, -1.0), 1.0));    // the window's edge
    CHECK(near(s.gain(55.0, -1.0), 0.625));  // halfway: 0.25 + 0.75 * 0.5
    CHECK(near(s.gain(57.5, -1.0), 0.4375)); // a quarter left: 0.25 + 0.75 * 0.25
    CHECK(near(s.gain(60.0, -1.0), 0.25));   // expiry: the floor
    CHECK(near(s.gain(75.0, -1.0), 0.25));   // and held there until the boundary stops it
    // A pause inside the window holds the fade where it was.
    s.pause(55.0);
    CHECK(near(s.gain(500.0, -1.0), 0.625));

    // End of chapter fades on the caller's estimate of the seconds left; unknown (<0) means no fade.
    ReadAloud::SleepSession c;
    CHECK(c.arm(chapterTimer(), 0.0, 1, false));
    CHECK(near(c.gain(0.0, 20.0), 1.0));
    CHECK(near(c.gain(0.0, 5.0), 0.625));
    CHECK(near(c.gain(0.0, 0.0), 0.25));
    CHECK(near(c.gain(0.0, -1.0), 1.0));

    // The estimate: 15 characters a second at 1x, faster with the speed, and never negative.
    CHECK(near(ReadAloud::estimateSpeakingSeconds(150, 1.0), 10.0));
    CHECK(near(ReadAloud::estimateSpeakingSeconds(150, 2.0), 5.0));
    CHECK(near(ReadAloud::estimateSpeakingSeconds(150, 0.0), 10.0));   // a nonsense speed reads as 1x
    CHECK(near(ReadAloud::estimateSpeakingSeconds(0, 1.0), 0.0));
    QVector<ReadAloud::Utterance> plan;
    plan.append(ReadAloud::Utterance{ 0, 30, QString(30, QLatin1Char('a')) });
    plan.append(ReadAloud::Utterance{ 31, 91, QString(60, QLatin1Char('b')) });
    plan.append(ReadAloud::Utterance{ 92, 182, QString(90, QLatin1Char('c')) });
    CHECK(near(ReadAloud::chapterSecondsLeft(plan, 1, 2.0, 1.0), 8.0));   // (60 + 90) / 15 - 2
    CHECK(near(ReadAloud::chapterSecondsLeft(plan, -1, 0.0, 1.0), 12.0)); // nothing spoken yet: all 180
    CHECK(near(ReadAloud::chapterSecondsLeft(plan, 2, 99.0, 1.0), 0.0));  // clamped, never negative
}

static void testSleepDisarms()
{
    // The user's own Stop.
    ReadAloud::SleepSession s;
    CHECK(s.arm(minutesTimer(1), 0.0, 0, false));
    s.narrationStopped();
    CHECK(!s.armed());
    CHECK(s.mode() == SleepTimer::Mode::Off);
    CHECK(!s.due(1000.0));
    CHECK(!s.stopAtBoundary(1000.0, 9));
    CHECK(near(s.gain(1000.0, 0.0), 1.0));
    s.resume(1000.0);                     // narration starting again does not bring the timer back
    CHECK(!s.armed());
    CHECK(!s.due(5000.0));

    // Leaving the book.
    ReadAloud::SleepSession b;
    CHECK(b.arm(chapterTimer(), 0.0, 3, false));
    b.bookLeft();
    CHECK(!b.armed());
    CHECK(!b.stopAtBoundary(10.0, 4));

    // Re-arming is a fresh session: its minutes count from the new arm, not the old one.
    ReadAloud::SleepSession r;
    CHECK(r.arm(minutesTimer(1), 0.0, 0, false));
    CHECK(r.arm(minutesTimer(1), 50.0, 0, false));
    CHECK(!r.due(100.0));
    CHECK(r.due(110.0));
}

static void testSleepOffDoesNothing()
{
    ReadAloud::SleepSession s;
    CHECK(!s.arm(offTimer(), 0.0, 0, false));
    CHECK(!s.armed());
    CHECK(!s.due(36000.0));
    CHECK(!s.stopAtBoundary(36000.0, 0));
    CHECK(!s.stopAtBoundary(36000.0, 1));   // not even across a chapter
    CHECK(near(s.gain(36000.0, 0.0), 1.0));
    CHECK(near(s.secondsLeft(36000.0, 5.0), -1.0));
    // A zero-minute Custom is not a timer either.
    CHECK(!s.arm(minutesTimer(0), 0.0, 0, false));
    CHECK(!s.armed());
    // Off from the menu over an armed timer turns it off.
    CHECK(s.arm(minutesTimer(15), 0.0, 0, false));
    CHECK(!s.arm(offTimer(), 1.0, 0, false));
    CHECK(!s.armed());
    CHECK(!s.stopAtBoundary(99999.0, 1));
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
    testVoiceChoice();
    testSleepMinutesExcludePause();
    testSleepEndOfChapterAtTheBoundary();
    testSleepExpiryWaitsForTheUtterance();
    testSleepFadeSchedule();
    testSleepDisarms();
    testSleepOffDoesNothing();
    if (failures == 0) std::printf("READALOUD-OK\n");
    return failures == 0 ? 0 : 1;
}
