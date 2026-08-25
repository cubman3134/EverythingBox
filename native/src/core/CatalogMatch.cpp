#include "CatalogMatch.h"
#include <QRegularExpression>
#include <QStringList>
#include <QtGlobal>

namespace CatalogMatch
{
QString normalizeTitle(const QString& t)
{
    // Fold diacritics: NFKD decomposes "é" → "e" + combining accent; drop the combining marks so the base
    // ASCII letter survives the [a-z0-9] filter ("Amélie" → "amelie", "Pokémon" → "pokemon").
    QString d = t.normalized(QString::NormalizationForm_KD).toLower();
    QString s; s.reserve(d.size());
    for (const QChar& ch : d)
        if (ch.category() != QChar::Mark_NonSpacing) s += ch;
    static const QRegularExpression nonAlnum(QStringLiteral("[^a-z0-9]+"));
    s.replace(nonAlnum, QStringLiteral(" "));
    s = s.simplified();
    for (const QString& art : { QStringLiteral("the "), QStringLiteral("a "), QStringLiteral("an ") })
        if (s.startsWith(art)) { s = s.mid(art.size()); break; }
    return s;
}

bool titleMatchesRequest(const QString& wantTitle, const QString& candidateTitle)
{
    // A candidate is judged on what it calls ITSELF, which is the part before its subtitle. Containment over
    // the whole string accepts any work that merely NAMES the one asked for — a parody, a study guide, an
    // "inspired by" — because the wanted title really is present in the text. Asking for Alice's Adventures in
    // Wonderland returned "Alice in Zombieland: Lewis Carroll's 'Alice's Adventures in Wonderland' with Undead
    // Madness", which contains it word for word and is a different book.
    //
    // Split on the RAW string: normalizing turns every colon into a space, so by then the subtitle is
    // indistinguishable from the title. Only a colon splits — a dash does not, because providers routinely
    // lead with the author ("Charlotte Bronte - Jane Eyre"), and cutting there would reject the real book.
    QString head = candidateTitle;
    const int colon = head.indexOf(QLatin1Char(':'));
    if (colon > 0) head = head.left(colon);

    const QString want = normalizeTitle(wantTitle);
    const QString cand = normalizeTitle(head);
    if (want.isEmpty() || cand.isEmpty()) return false;   // nothing to compare is not a match

    // Padded so containment lands on whole tokens: without it "it" matches "commitment", and a two-letter
    // title is exactly the case where a loose rule does the most damage.
    const QString w = QStringLiteral(" ") + want + QStringLiteral(" ");
    const QString c = QStringLiteral(" ") + cand + QStringLiteral(" ");
    return c.contains(w) || w.contains(c);
}

// ---- ROM dump names (games only) ---------------------------------------------------------------
// See the header for WHY none of this may reach a book. Everything below is reachable only from
// normalizeRomTitle / gameTitleMatchesRequest, and the doc-bridge calls those only for a `game` catalog.

// The archive and ROM-image extensions a dump is published under. An explicit list, not "whatever follows the
// last dot": the generic rule eats the real end of a title (nothing in it can tell "Mr. Do" from "Game.bin"),
// and a title it eats is a game the user cannot download. Anchored to the end, so a dot INSIDE a title is
// never touched.
static const QRegularExpression& romExtRe()
{
    static const QRegularExpression re(
        QStringLiteral("\\.(?:7z|zip|rar|gz|bz2|xz"
                       "|nes|fds|unf|unif|sfc|smc|swc|fig|gb|gbc|gba|srl"
                       "|n64|z64|v64|nds|dsi|3ds|cci|cia|cxi"
                       "|iso|cue|bin|img|chd|cdi|gdi|rvz|wbfs|gcm|gcz|nkit|cso|pbp"
                       "|md|smd|gen|sms|gg|sg|32x|pce|sgx"
                       "|a26|a52|a78|lnx|ngp|ngc|ws|wsc|col|int|vec|vb|min|j64|jag"
                       "|xci|nsp|pkg|vpk|adf|adz|d64|t64|tap|dsk|st|msa|atr|xex|rom|prg)$"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// A trailing tag group: "(USA)", "(Rev 1)", "(En,Fr,De)", "[!]", "[U]", "[T+Eng1.0]". TRAILING only, and one
// group at a time so a stack of them peels: dump tools APPEND these, and the same brackets in the middle of a
// name are part of the title. Stripping "(Rev 1)" before the sequel check below is load-bearing - otherwise
// its "1" reads as a sequel number and refuses the very ROM it is a revision of.
static const QRegularExpression& romTagRe()
{
    static const QRegularExpression re(QStringLiteral("\\s*[\\(\\[][^\\(\\)\\[\\]]*[\\)\\]]\\s*$"));
    return re;
}

// The article the dump convention moves to the END after a comma: "Legend of Zelda, The - A Link to the Past",
// "Adventures of Batman & Robin, The". Dropped rather than moved back, because normalizeTitle already drops a
// LEADING article - so both spellings land on the same key.
//
// CONDITIONED, in the same spirit as the same problem in PcGameId.cpp:61 ("a leftover trailing article,
// dropped ONLY when an edition phrase was actually stripped"): the article counts only when it is followed by
// the END of the string or a subtitle separator, which is the whole of the convention. That lookahead is what
// leaves "Zelda, The Adventure of Link" - a sentence, not a dump name - intact, so it cannot collapse onto
// "Zelda II: The Adventure of Link" and hand a romhack the wrong base ROM.
//
// PcGameId's own regex is NOT reused. It carries no comma, fires only at the very end of the string, and is
// gated on an edition-phrase strip that has no counterpart here - so it matches none of the three shapes above
// while matching titles that genuinely end in an article, which this must not. It also lives in a QSettings /
// AppPaths translation unit whose merge-verdict store this pure matcher has no business linking. What is
// reused is its rule: never drop an article without evidence that it is noise.
static const QRegularExpression& romTrailingArticleRe()
{
    static const QRegularExpression re(
        QStringLiteral(",\\s+(?:the|a|an)(?=\\s*(?:$|[-:\\x{2013}\\x{2014}]))"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// Is this normalized token a sequel marker? Arabic numerals, and roman numerals from II up. Bare "i" is
// deliberately NOT one: "Final Fantasy I" and "Final Fantasy" are the same game, so treating "i" as a sequel
// would refuse a real match, while nothing is at risk the other way ("II" upwards is what separates sequels).
static bool isSequelToken(const QString& t)
{
    if (t.isEmpty()) return false;
    bool digits = true;
    for (const QChar& ch : t) if (!ch.isDigit()) { digits = false; break; }
    if (digits) return true;
    static const QStringList roman{ QStringLiteral("ii"),   QStringLiteral("iii"), QStringLiteral("iv"),
                                    QStringLiteral("v"),    QStringLiteral("vi"),  QStringLiteral("vii"),
                                    QStringLiteral("viii"), QStringLiteral("ix"),  QStringLiteral("x"),
                                    QStringLiteral("xi"),   QStringLiteral("xii"), QStringLiteral("xiii") };
    return roman.contains(t);
}

QString normalizeRomTitle(const QString& t)
{
    QString s = t.trimmed();
    // Bounded and shortening: each pass either removes something or stops. Extension first (it is outermost),
    // then the tag stack it was hiding, then round again for "Game (USA).nes.zip".
    for (int pass = 0; pass < 8; ++pass)
    {
        const QString before = s;
        const QRegularExpressionMatch ext = romExtRe().match(s);
        // Only when something is LEFT of it: ".zip" alone is not a title carrying an extension.
        if (ext.hasMatch() && ext.capturedStart(0) > 0) s.truncate(ext.capturedStart(0));
        for (int tag = 0; tag < 8; ++tag)
        {
            const QRegularExpressionMatch m = romTagRe().match(s);
            if (!m.hasMatch()) break;
            s.truncate(m.capturedStart(0));
        }
        if (s == before) break;
    }
    s.remove(romTrailingArticleRe());
    return normalizeTitle(s);
}

bool gameTitleMatchesRequest(const QString& wantTitle, const QString& candidateTitle)
{
    // The candidate is judged on what it calls ITSELF, before its subtitle - the same rule, and for the same
    // reason, as titleMatchesRequest. Split on the RAW string: by the time normalizeRomTitle has run, a colon
    // is a space like any other. A dump name normally separates with " - " and has no colon to cut at, so this
    // is for the catalogs that write one.
    const QString head = candidateTitle.trimmed();
    const QString want = normalizeRomTitle(wantTitle);
    QString cand = normalizeRomTitle(head);
    const int colon = head.indexOf(QLatin1Char(':'));
    if (colon > 0)
    {
        const QString h = normalizeRomTitle(head.left(colon));
        if (!h.isEmpty()) cand = h;
    }
    if (want.isEmpty() || cand.isEmpty()) return false;   // nothing to compare is not a match

    const QStringList wt = want.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QStringList ct = cand.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (wt.isEmpty() || ct.isEmpty()) return false;
    const QStringList& shortT = (wt.size() <= ct.size()) ? wt : ct;
    const QStringList& longT  = (wt.size() <= ct.size()) ? ct : wt;

    // Whole tokens, either side may be the longer one - containment, done over token RUNS rather than padded
    // substrings so that the token immediately after the run can be inspected.
    bool found = false;
    for (int i = 0; i + shortT.size() <= longT.size(); ++i)
    {
        bool run = true;
        for (int j = 0; j < shortT.size(); ++j)
            if (longT.at(i + j) != shortT.at(j)) { run = false; break; }
        if (!run) continue;
        // THE ONE EXTRA REFUSAL, and the whole reason this is not just titleMatchesRequest over a different
        // normalizer. A sequel marker directly after the matched run is the number being dropped: "Zelda" |
        // "ii the adventure of link", "Mega Man" | "2", "Final Fantasy" | "vi". Refused on ANY occurrence
        // rather than hunting for a cleaner one - see the header for what a wrong base ROM costs. A number
        // BEFORE the run is not this case ("Yoshi's Island" is genuinely inside "Super Mario World 2 - Yoshi's
        // Island"), and neither is one further along ("Donkey Kong Country 2 - Diddy's Kong Quest").
        if (i + shortT.size() < longT.size() && isSequelToken(longT.at(i + shortT.size()))) return false;
        found = true;
    }
    return found;
}

QString localCopyFor(const QString& wantId, const QString& wantTitle, const QString& wantKind,
                     const QVector<LocalCopy>& have)
{
    // The key is what the copy was SAVED under, so it identifies the work exactly. Taken first and on its own
    // — a key match needs no agreement from the title, which a re-titled catalog row would not give.
    if (!wantId.isEmpty())
        for (const LocalCopy& c : have)
            if (!c.key.isEmpty() && c.key.compare(wantId, Qt::CaseInsensitive) == 0 && !c.path.isEmpty())
                return c.path;

    const QString want = normalizeTitle(wantTitle);
    if (want.isEmpty() || wantKind.isEmpty()) return QString();

    for (const LocalCopy& c : have)
    {
        if (c.path.isEmpty()) continue;
        if (c.kind.compare(wantKind, Qt::CaseInsensitive) != 0) continue;   // never across kinds
        if (normalizeTitle(c.title) != want) continue;                      // exact, not contained
        return c.path;
    }
    return QString();
}

static int yearFromSubtitle(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("\\b(19|20)\\d{2}\\b"));
    const QRegularExpressionMatch m = re.match(s);
    return m.hasMatch() ? m.captured(0).toInt() : 0;
}

int bestMatch(const LocalLibrary::VideoEntry& want, const QVector<MediaItem>& candidates)
{
    if (!want.imdbId.isEmpty())
        for (int i = 0; i < candidates.size(); ++i)
            if (candidates[i].id.compare(want.imdbId, Qt::CaseInsensitive) == 0)
                return i;

    const QString wt = normalizeTitle(want.title);
    if (wt.isEmpty()) return -1;

    int hit = -1;
    for (int i = 0; i < candidates.size(); ++i)
    {
        const MediaItem& c = candidates[i];
        if (!c.type.isEmpty() && c.type != QStringLiteral("movie")) continue; // not a same-named series/etc.
        // A candidate with a KNOWN imdb id ("tt…") that differs from the NFO's imdb is an
        // affirmatively-wrong film sharing the title — never accept it via the title path.
        // (An exact imdb match was already accepted at the top of this function.)
        if (!want.imdbId.isEmpty()
            && c.id.startsWith(QStringLiteral("tt"), Qt::CaseInsensitive)
            && c.id.compare(want.imdbId, Qt::CaseInsensitive) != 0) continue;
        if (normalizeTitle(c.title) != wt) continue;
        // Subtitle-year disambiguation: when we know the local year and the candidate advertises a year
        // (aiocatalog puts it in the search row's subtitle), they must agree within ±1 — else this is a
        // same-title film of a different year (e.g. Solaris 1972 vs 2002). Skip it. Candidates with no
        // parseable subtitle year are left as title matches (unchanged fallback).
        if (want.year > 0)
        {
            const int cy = yearFromSubtitle(c.subtitle);
            if (cy > 0 && qAbs(cy - want.year) > 1) continue;
        }
        if (hit != -1) return -1;   // ambiguous: more than one title match
        hit = i;
    }
    return hit;
}

QString requestedChapterNumber(const QString& query)
{
    // The number the caller appended to the parent title, at the very end of the query. Allow a fractional
    // part ("1052.5") but anchor to the end so a number buried inside the title ("7 Deadly Sins") is not
    // mistaken for a chapter request.
    static const QRegularExpression tail(QStringLiteral("(\\d+(?:\\.\\d+)?)\\s*$"));
    const QRegularExpressionMatch m = tail.match(query);
    return m.hasMatch() ? m.captured(1) : QString();
}

bool chapterNumberMatches(const QString& itemTitle, const QString& want)
{
    if (want.isEmpty()) return false;
    static const QRegularExpression num(QStringLiteral("(\\d+(?:\\.\\d+)?)"));
    const QRegularExpressionMatch m = num.match(itemTitle);
    if (!m.hasMatch()) return false;
    bool okA = false, okB = false;
    const double a = m.captured(1).toDouble(&okA);
    const double b = want.toDouble(&okB);
    if (!okA || !okB) return false;
    return qAbs(a - b) < 1e-9;    // numeric compare: "1" == "1.0", "12" != "1"
}

QString docCatalogSibling(const QString& type)
{
    if (type == QStringLiteral("comic")) return QStringLiteral("manga");
    if (type == QStringLiteral("manga")) return QStringLiteral("comic");
    return QString();
}

int bestSeriesMatch(const QString& showTitle, const QString& seriesImdbId, const QVector<MediaItem>& candidates)
{
    if (!seriesImdbId.isEmpty())
        for (int i = 0; i < candidates.size(); ++i)
            if (candidates[i].id.compare(seriesImdbId, Qt::CaseInsensitive) == 0) return i;

    const QString wt = normalizeTitle(showTitle);
    if (wt.isEmpty()) return -1;

    int hit = -1;
    for (int i = 0; i < candidates.size(); ++i)
    {
        const MediaItem& c = candidates[i];
        if (c.type != QStringLiteral("series") && c.type != QStringLiteral("tv")) continue; // series only
        if (!seriesImdbId.isEmpty()
            && c.id.startsWith(QStringLiteral("tt"), Qt::CaseInsensitive)
            && c.id.compare(seriesImdbId, Qt::CaseInsensitive) != 0) continue;              // contradicted tt
        if (normalizeTitle(c.title) != wt) continue;
        if (hit != -1) return -1;                                                           // ambiguous
        hit = i;
    }
    return hit;
}
}
