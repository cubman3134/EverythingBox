#include "ComicName.h"

#include <QHash>
#include <QRegularExpression>

namespace ComicName
{
namespace
{
    // The characters a series name may be left dangling on once a marker has been cut out of the middle of
    // a name — "Saga, " from "Saga, Vol. 1", "Batman - " from "Batman - v02". A trailing '.' is deliberately
    // NOT in this set: "Mr." and "St." end real names, and the marker shapes consume their own dot.
    bool isDangler(QChar c)
    {
        return c.isSpace() || c == QLatin1Char('-') || c == QChar(0x2013) || c == QChar(0x2014)
            || c == QLatin1Char('_') || c == QLatin1Char(',') || c == QLatin1Char(':')
            || c == QLatin1Char(';') || c == QLatin1Char('#');
    }

    QString trimDanglers(QString s)
    {
        while (!s.isEmpty() && isDangler(s.at(s.size() - 1))) s.chop(1);
        while (!s.isEmpty() && isDangler(s.at(0)))            s.remove(0, 1);
        return s;
    }

    // A series name has to be something a person could have written. Without this, "01 - 02" reads as the
    // series "01" and every numbered file in a folder of loose scans joins it.
    bool hasLetter(const QString& s)
    {
        for (const QChar& c : s) if (c.isLetter()) return true;
        return false;
    }

    // THE YEAR REFUSAL. Four digits inside the window people actually publish in are a year, not an issue
    // number — see the header. Applied to every shape, including the marked ones: "Batman - 1989" is a year
    // wearing a dash-field's clothes.
    bool looksLikeYear(const QString& digits)
    {
        if (digits.size() != 4) return false;
        const int v = digits.toInt();
        return v >= 1900 && v <= 2099;
    }

    // Trailing bracketed groups only, and only whole balanced ones. "(2013) (Digital) (Zone-Empire)" comes
    // off; "Batman (Earth-Two)" as a whole name does not, because removing it would leave nothing.
    QString stripTrailingGroups(const QString& in)
    {
        QString s = in.trimmed();
        forever
        {
            if (s.isEmpty()) break;
            const QChar close = s.at(s.size() - 1);
            QChar open;
            if      (close == QLatin1Char(')')) open = QLatin1Char('(');
            else if (close == QLatin1Char(']')) open = QLatin1Char('[');
            else if (close == QLatin1Char('}')) open = QLatin1Char('{');
            else break;

            const int at = s.lastIndexOf(open);
            if (at < 0) break;                       // an unbalanced ')' is part of the name
            const QString head = s.left(at).trimmed();
            if (head.isEmpty()) break;               // stripping would leave nothing: keep the whole name
            s = head;
        }
        return s;
    }

    // Underscores and runs of whitespace become single spaces. This is the ONLY text rewriting done before
    // the shapes below are tried: an underscore is never meaningful inside a comic's title and is what half
    // the scrapers in the world use as a space, while anything more aggressive (dropping punctuation,
    // folding "Spider-Man") is the class of normalisation this file exists to refuse.
    QString tidy(const QString& in)
    {
        QString s = in;
        s.replace(QLatin1Char('_'), QLatin1Char(' '));
        return s.simplified();
    }

    Parsed marked(const QString& series, const QString& digits, const QString& title, const QString& cleaned)
    {
        Parsed p;
        p.cleaned  = cleaned;
        p.series   = trimDanglers(series);
        p.number   = digits.toDouble();
        p.title    = trimDanglers(title);
        p.evidence = Evidence::Marked;
        return p;
    }

    Parsed none(const QString& cleaned)
    {
        Parsed p;
        p.cleaned = cleaned;
        return p;
    }
}

QString seriesKey(const QString& series)
{
    // Case and whitespace only. See the header: folding punctuation here would quietly merge series whose
    // names differ by a hyphen, and merging is the expensive failure.
    return series.simplified().toCaseFolded();
}

Parsed parse(const QString& baseName)
{
    const QString cleaned = tidy(stripTrailingGroups(tidy(baseName)));
    if (cleaned.isEmpty()) return none(tidy(baseName));

    // ---- 1. A '#'. The least ambiguous marker there is: nobody writes a '#' before a number that is not
    // an issue number. ------------------------------------------------------------------------------------
    static const QRegularExpression hashRe(QStringLiteral("^(.*?)#\\s*(\\d{1,4})(?:\\.(\\d{1,2}))?\\s*(.*)$"));
    if (const QRegularExpressionMatch m = hashRe.match(cleaned); m.hasMatch())
    {
        const QString whole = m.captured(2);
        if (!looksLikeYear(whole) || !m.captured(3).isEmpty())
        {
            const QString series = trimDanglers(m.captured(1));
            if (hasLetter(series))
            {
                const QString digits = m.captured(3).isEmpty() ? whole
                                                               : whole + QLatin1Char('.') + m.captured(3);
                return marked(series, digits, m.captured(4), cleaned);
            }
        }
    }

    // ---- 2. A VOLUME word or a v01 token. Two things here are load-bearing and both were chosen against a
    // real filename. `\b` before the `v` is what keeps "Evolution 5" and "TV5" from carrying a volume marker
    // they never meant. And the BARE `v` form takes its digits with NO space between — `v01`, `v1`, never
    // "v 1" — because "Batman v Superman 2" and "Alien v Predator 3" spell a versus as a lone letter and a
    // space, and reading those as volume 2 and volume 3 would file two films' one-shots under the wrong
    // series with a marker's confidence. The spelled-out `Vol`/`Volume` may space its number, because no
    // English title uses that word as a preposition. ---------------------------------------------------
    static const QRegularExpression volRe(
        QStringLiteral("^(.*?)\\b(?:(?:vol\\.?|volume)\\s*|v)(\\d{1,4})\\b\\s*(.*)$"),
        QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch m = volRe.match(cleaned); m.hasMatch())
    {
        const QString series = trimDanglers(m.captured(1));
        if (hasLetter(series) && !looksLikeYear(m.captured(2)))
            return marked(series, m.captured(2), m.captured(3), cleaned);
    }

    // ---- 3. A number standing alone as its own DASH-DELIMITED FIELD: "Series - 012" and
    // "Series - 012 - Title". The separators are the statement — a person who writes a number as a field
    // between dashes has said it is not part of the name either side of it. -------------------------------
    static const QRegularExpression fieldRe(
        QStringLiteral("^(.+?)\\s+-\\s+(\\d{1,4})(?:\\.(\\d{1,2}))?\\s*(?:-\\s*(.+))?$"));
    if (const QRegularExpressionMatch m = fieldRe.match(cleaned); m.hasMatch())
    {
        const QString whole = m.captured(2);
        const QString series = trimDanglers(m.captured(1));
        if (hasLetter(series) && (!looksLikeYear(whole) || !m.captured(3).isEmpty()))
        {
            const QString digits = m.captured(3).isEmpty() ? whole : whole + QLatin1Char('.') + m.captured(3);
            return marked(series, digits, m.captured(4), cleaned);
        }
    }

    // ---- 4. A BARE number: "Saga 012", and "Saga 012 - The Title" where the number is delimited on ONE
    // side only. Graded, never believed on its own — group() asks the folder whether it agrees. -----------
    static const QRegularExpression bareRe(
        QStringLiteral("^(.+?)\\s+(\\d{1,4})(?:\\.(\\d{1,2}))?(?:\\s*-\\s*(.+))?$"));
    if (const QRegularExpressionMatch m = bareRe.match(cleaned); m.hasMatch())
    {
        const QString whole  = m.captured(2);
        const QString series = trimDanglers(m.captured(1));
        if (hasLetter(series) && (!looksLikeYear(whole) || !m.captured(3).isEmpty()))
        {
            Parsed p;
            p.cleaned  = cleaned;
            p.series   = series;
            p.number   = (m.captured(3).isEmpty() ? whole : whole + QLatin1Char('.') + m.captured(3))
                             .toDouble();
            p.title    = trimDanglers(m.captured(4));
            p.evidence = Evidence::Bare;
            return p;
        }
    }

    return none(cleaned);
}

QVector<Grouped> group(const QStringList& baseNames)
{
    QVector<Parsed> parsed;
    parsed.reserve(baseNames.size());
    for (const QString& b : baseNames) parsed.push_back(parse(b));

    // How many files in this folder read as the same BARE series. Marked files are deliberately NOT counted
    // towards it: the question a bare number is asking is "does anything else here agree that this prefix is
    // a series", and a file that already carries a marker has answered a different question — it is grouped
    // whatever its neighbours say, and letting it vouch for a bare sibling would make "Saga Vol. 1" enough
    // to turn a lone "Saga 451" into an issue. That is exactly one corroborator too few.
    QHash<QString, int> bareCount;
    for (const Parsed& p : parsed)
        if (p.evidence == Evidence::Bare) ++bareCount[seriesKey(p.series)];

    QVector<Grouped> out;
    out.reserve(parsed.size());
    for (const Parsed& p : parsed)
    {
        Grouped g;
        // The display title, and it is NEVER empty: a file whose name told us nothing still has its name.
        g.title = p.title.isEmpty() ? p.cleaned : p.title;

        const bool believed =
            p.evidence == Evidence::Marked
            || (p.evidence == Evidence::Bare
                && bareCount.value(seriesKey(p.series)) >= kBareCorroboration);
        if (believed)
        {
            g.series = p.series;
            g.number = p.number;
        }
        else
        {
            // Ungrouped: the whole cleaned name is the title, INCLUDING the number we declined to read.
            // "Fahrenheit 451" must not appear on a shelf as "Fahrenheit".
            g.title = p.cleaned;
        }
        out.push_back(g);
    }
    return out;
}

} // namespace ComicName
