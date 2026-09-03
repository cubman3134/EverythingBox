#include "ReadAloud.h"
#include <QRegularExpression>
#include <QSet>

namespace
{

// Invisible characters an engine has no business pronouncing, and NBSP (which becomes an ordinary space so the
// whitespace collapse below can see it).
const QRegularExpression& reInvisible()
{
    static const QRegularExpression re(QStringLiteral("[\\x{00AD}\\x{200B}-\\x{200D}\\x{FEFF}]"));
    return re;
}

// Bracketed reference numbers and bracketed page artifacts: "[12]", "[3, 4]", "[7-9]", "[Page 12]", "[Pg 4]",
// "[p. 12]". The body is restricted to a page word plus digits, commas, spaces and dashes, so an EDITORIAL
// bracket — "[sic]", "[He laughed]" — is prose and survives untouched.
const QRegularExpression& reBracketRef()
{
    static const QRegularExpression re(
        QStringLiteral("\\[\\s*(?:p(?:g|age|\\.)?\\s*)?\\d+(?:\\s*[-\\x{2013}\\x{2014},]\\s*\\d+)*\\s*\\]"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// Superscript digit footnote markers, and the footnote symbols. The asterisk goes with them: as a marker, a
// scene-break rule or Markdown emphasis it is never a thing to say out loud.
const QRegularExpression& reMarkers()
{
    static const QRegularExpression re(
        QStringLiteral("[\\x{00B9}\\x{00B2}\\x{00B3}\\x{2070}-\\x{2079}\\x{2020}\\x{2021}\\x{00A7}\\x{2016}\\x{00B6}*]"));
    return re;
}

const QRegularExpression& reWhitespace()
{
    static const QRegularExpression re(QStringLiteral("\\s+"));
    return re;
}

// A whole paragraph that is nothing but a page number: "12", "— 12 —", "- 4 -", "(23)", "Page 199", "[Pg 4]".
// Arabic digits only, and the WHOLE paragraph — so "see page 12" inside a sentence and a roman-numeral chapter
// head ("II") are both left alone.
const QRegularExpression& rePageArtifact()
{
    static const QRegularExpression re(
        QStringLiteral("^[\\[({]?\\s*[-\\x{2013}\\x{2014}]*\\s*(?:p(?:g|age|\\.)?\\s*)?\\d+\\s*[-\\x{2013}\\x{2014}]*\\s*[\\])}]?$"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

bool isTerminator(QChar c)
{
    return c == QLatin1Char('.') || c == QLatin1Char('!') || c == QLatin1Char('?') || c == QChar(0x2026);
}

// Closing marks that ride ALONG with a terminator and still belong to the sentence that ended: `He said "Go."`
// ends after the quote, not before it.
bool isClosing(QChar c)
{
    switch (c.unicode())
    {
    case '"': case '\'': case ')': case ']': case '}':
    case 0x201D: case 0x2019: case 0x00BB:   // ” ’ »
        return true;
    default:
        return false;
    }
}

// What can begin the NEXT sentence. A capital or a digit, an opening quote or bracket, or a dash (dialogue in
// many translations opens on one). Anything else — most importantly a lowercase letter — means the terminator
// was internal: `"Stop!" he said.` is one sentence, and dividing it would read as two.
bool canStartSentence(QChar c)
{
    if (c.isUpper() || c.isDigit()) return true;
    switch (c.unicode())
    {
    case '"': case '\'': case '(': case '[': case '{':
    case 0x201C: case 0x2018: case 0x00AB:            // “ ‘ «
    case 0x2014: case 0x2013:                         // — –
        return true;
    default:
        return false;
    }
}

// Abbreviations whose trailing '.' is part of the word, not the end of a sentence. Deliberately conservative:
// only tokens that are not themselves English words, so a real sentence ending in that word is never swallowed.
// ("No." is left OUT for exactly that reason — "He said no. Then he left." is two sentences.)
const QSet<QString>& abbreviations()
{
    static const QSet<QString> s = {
        QStringLiteral("mr"),    QStringLiteral("mrs"),  QStringLiteral("ms"),    QStringLiteral("dr"),
        QStringLiteral("prof"),  QStringLiteral("st"),   QStringLiteral("jr"),    QStringLiteral("sr"),
        QStringLiteral("vs"),    QStringLiteral("etc"),  QStringLiteral("fig"),   QStringLiteral("figs"),
        QStringLiteral("vol"),   QStringLiteral("vols"), QStringLiteral("ch"),    QStringLiteral("chap"),
        QStringLiteral("rev"),   QStringLiteral("hon"),  QStringLiteral("capt"),  QStringLiteral("lt"),
        QStringLiteral("col"),   QStringLiteral("gen"),  QStringLiteral("sgt"),   QStringLiteral("mt"),
        QStringLiteral("ave"),   QStringLiteral("inc"),  QStringLiteral("ltd"),   QStringLiteral("co"),
        QStringLiteral("corp"),  QStringLiteral("cf"),   QStringLiteral("al"),    QStringLiteral("viz"),
        QStringLiteral("ibid"),  QStringLiteral("op"),   QStringLiteral("messrs"),QStringLiteral("esq"),
        QStringLiteral("dept"),  QStringLiteral("univ"), QStringLiteral("approx"),QStringLiteral("pp"),
    };
    return s;
}

// Is the '.' at `dot` part of an abbreviation or an initial rather than a sentence end? The letters immediately
// before it are the token: one letter is an INITIAL ("J. R. R. Tolkien", and the 'g' of "e.g."), and a token in
// the table is an abbreviation. An empty token — a digit or another '.' sits there — is not either: "born in
// 1892. Then" really does end a sentence.
bool isAbbreviationOrInitial(const QString& s, int dot)
{
    int b = dot;
    while (b > 0 && s.at(b - 1).isLetter()) --b;
    const int len = dot - b;
    if (len == 0) return false;
    if (len == 1) return true;                       // an initial
    return abbreviations().contains(s.mid(b, len).toLower());
}

// Break inside an over-long sentence WITHOUT splitting a word: the last whitespace at or before `limit`. Falls
// back to `limit` itself only when there is no whitespace at all in the window (a 600-character URL), which is
// the one case where a hard break beats never making progress.
int breakAtWord(const QString& s, int from, int limit)
{
    for (int i = limit; i > from; --i)
        if (s.at(i - 1).isSpace()) return i;
    return limit > from ? limit : from + 1;
}

} // namespace

namespace ReadAloud
{

// The exclusive END offsets of every sentence in `s`, ascending, always closing with s.size(). The rules are
// spelled out in ReadAloud.h; this is their one implementation.
QVector<int> sentenceEnds(const QString& s)
{
    QVector<int> cuts;
    const int n = s.size();
    for (int i = 0; i < n; ++i)
    {
        if (!isTerminator(s.at(i))) continue;

        int j = i;                                              // a run of terminators: "?!", "..."
        while (j + 1 < n && isTerminator(s.at(j + 1))) ++j;
        int k = j + 1;                                          // then any closing quotes/brackets
        while (k < n && isClosing(s.at(k))) ++k;

        // The next character has to be whitespace (or the paragraph has to end). "3.14" and "etc.," are not
        // boundaries, and this is the clause that says so.
        if (k < n && !s.at(k).isSpace()) { i = j; continue; }

        int m = k;
        while (m < n && s.at(m).isSpace()) ++m;
        if (m < n && !canStartSentence(s.at(m))) { i = j; continue; }

        // Only a LONE '.' can be an abbreviation; "etc..." or "!." already ended something.
        if (s.at(i) == QLatin1Char('.') && j == i && isAbbreviationOrInitial(s, i)) { i = j; continue; }

        cuts.append(k);
        i = k - 1;
    }
    if (cuts.isEmpty() || cuts.constLast() != n) cuts.append(n);
    return cuts;
}

QString stripArtifacts(const QString& raw)
{
    QString s = raw;
    s.remove(reBracketRef());
    s.remove(reMarkers());
    s.remove(reInvisible());
    s.replace(QChar(0x00A0), QLatin1Char(' '));
    s.replace(reWhitespace(), QStringLiteral(" "));
    return s.trimmed();
}

bool isPageArtifact(const QString& rawParagraph)
{
    QString s = rawParagraph;
    s.remove(reInvisible());
    s.replace(QChar(0x00A0), QLatin1Char(' '));
    s = s.trimmed();
    if (s.isEmpty()) return false;              // a blank line is not a page number; it is simply nothing
    return rePageArtifact().match(s).hasMatch();
}

QVector<Utterance> divideParagraph(const QString& paragraph, int paragraphStart, int maxChars)
{
    QVector<Utterance> out;
    if (maxChars <= 0) maxChars = kLongParagraphChars;
    if (isPageArtifact(paragraph)) return out;

    const int n = paragraph.size();
    if (n == 0) return out;

    // The common case: a paragraph is one utterance. Prose is written in paragraphs, and an engine given a
    // paragraph phrases it like one.
    if (n <= maxChars)
    {
        const QString text = stripArtifacts(paragraph);
        if (!text.isEmpty()) out.append(Utterance{ paragraphStart, paragraphStart + n, text });
        return out;
    }

    const QVector<int> cuts = sentenceEnds(paragraph);
    int segBegin = 0;
    int c = 0;
    while (segBegin < n)
    {
        // Skip cuts we have already passed (an over-long sentence was broken inside).
        while (c < cuts.size() && cuts[c] <= segBegin) ++c;
        if (c >= cuts.size()) break;

        int segEnd = segBegin;
        while (c < cuts.size() && (cuts[c] - segBegin) <= maxChars) { segEnd = cuts[c]; ++c; }
        if (segEnd == segBegin)                       // one sentence alone is longer than a chunk
            segEnd = breakAtWord(paragraph, segBegin, qMin(segBegin + maxChars, cuts[c]));

        // Trim the segment's own edges so the highlight starts and ends on a word.
        int b = segBegin, e = segEnd;
        while (b < e && paragraph.at(b).isSpace()) ++b;
        while (e > b && paragraph.at(e - 1).isSpace()) --e;
        if (e > b)
        {
            const QString text = stripArtifacts(paragraph.mid(b, e - b));
            if (!text.isEmpty()) out.append(Utterance{ paragraphStart + b, paragraphStart + e, text });
        }
        segBegin = segEnd;
    }
    return out;
}

QVector<Utterance> plan(const QString& chapterPlainText, int maxChars)
{
    QVector<Utterance> out;
    const int n = chapterPlainText.size();
    int pos = 0;
    while (pos <= n)
    {
        int end = pos;
        while (end < n)
        {
            const ushort u = chapterPlainText.at(end).unicode();
            if (u == '\n' || u == 0x2028 || u == 0x2029) break;   // QTextDocument writes '\n'; the separators
            ++end;                                                // are accepted too, and are one char wide
        }
        out += divideParagraph(chapterPlainText.mid(pos, end - pos), pos, maxChars);
        if (end >= n) break;
        pos = end + 1;
    }
    return out;
}

int indexForOffset(const QVector<Utterance>& utterances, int offset)
{
    if (utterances.isEmpty()) return -1;
    for (int i = 0; i < utterances.size(); ++i)
    {
        if (offset < utterances[i].start) return i;   // in the gap before it — start at the NEXT paragraph
        if (offset < utterances[i].end)   return i;   // inside it
    }
    return utterances.size() - 1;
}

ReaderAnchor anchorFor(int spine, const Utterance& u)
{
    ReaderAnchor a;
    a.kind   = ReaderAnchor::Book;
    a.spine  = spine;
    a.offset = u.start;
    return a;                                          // endOffset stays -1: a point, not a selection
}

int indexForAnchor(const QVector<Utterance>& utterances, const ReaderAnchor& a)
{
    if (a.kind != ReaderAnchor::Book) return -1;
    return indexForOffset(utterances, a.offset);
}

QVector<double> speedSteps()
{
    return { 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0 };
}

double nextSpeedStep(double speed)
{
    const QVector<double> steps = speedSteps();
    int nearest = 0;
    double bestDelta = qAbs(steps[0] - speed);
    for (int i = 1; i < steps.size(); ++i)
    {
        const double d = qAbs(steps[i] - speed);
        if (d < bestDelta) { bestDelta = d; nearest = i; }
    }
    return steps[(nearest + 1) % steps.size()];
}

double engineRateForSpeed(double speed)
{
    if (speed <= 0.0) return 0.0;                       // a corrupt/absent value reads as normal, never silent
    const double r = speed >= 1.0 ? (speed - 1.0)       // 1.0 -> 0.0, 2.0 -> +1.0
                                  : (speed - 1.0) * 2.0; // 0.75 -> -0.5, 0.5 -> -1.0
    return qBound(-1.0, r, 1.0);
}

int bookSettingsRowCount(bool readAloudAvailable)
{
    // Exit, font −, font +, theme, typeface — the five the book's row has always had. Read-aloud adds its
    // four (speak/stop, pause/resume, speed, voice) and NOTHING when the engine module is not in the build.
    return readAloudAvailable ? 9 : 5;
}

} // namespace ReadAloud
