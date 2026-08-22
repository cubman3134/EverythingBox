#include "CatalogMatch.h"
#include <QRegularExpression>
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
