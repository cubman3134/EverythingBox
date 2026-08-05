#include "GameFilter.h"

#include <QJsonArray>
#include <QRegularExpression>

namespace gamefilter
{
bool Filter::isEmpty() const
{
    return systems.isEmpty() && tags.isEmpty() && genres.isEmpty() && minPlayers.isEmpty()
        && decades.isEmpty() && completions.isEmpty() && favorite == Tri::Any && played == Tri::Any
        && hidden == Tri::Any;
}

// A tri-state constraint passes when it is Any, or when the fact equals the demanded truth. Factored out so
// all three booleans share one rule and a mutation to it is caught once, not three times.
static bool triOk(Tri want, bool fact)
{
    return want == Tri::Any || (want == Tri::Yes) == fact;
}

bool matches(const Filter& f, const GameFacts& g)
{
    // Each block below is one dimension. A present dimension that finds no accepting value fails the whole
    // match (AND across dimensions); within a dimension the first accepting value is enough (OR within).
    if (!f.systems.isEmpty())
    {
        bool ok = false;
        for (const QString& s : f.systems)
            if (g.systems.contains(s, Qt::CaseInsensitive)) { ok = true; break; }
        if (!ok) return false;
    }
    if (!f.tags.isEmpty())
    {
        bool ok = false;
        for (const QString& t : f.tags)
            if (g.tags.contains(t)) { ok = true; break; }
        if (!ok) return false;
    }
    if (!f.genres.isEmpty())
    {
        bool ok = false;
        for (const QString& want : f.genres)
        {
            for (const QString& have : g.genres)
                if (have.compare(want, Qt::CaseInsensitive) == 0) { ok = true; break; }
            if (ok) break;
        }
        if (!ok) return false;
    }
    if (!f.minPlayers.isEmpty())
    {
        // OR of ">= N" thresholds. A game with unknown player count (maxPlayers == 0) matches no threshold,
        // so it is filtered out — an unscraped game does not pretend to support any number of players.
        bool ok = false;
        for (int n : f.minPlayers)
            if (g.maxPlayers >= n && g.maxPlayers > 0) { ok = true; break; }
        if (!ok) return false;
    }
    if (!f.decades.isEmpty())
    {
        // Each decade value is its start year (1990 == the 1990s). Year 0 (unscraped) is in no decade.
        bool ok = false;
        if (g.releaseYear > 0)
            for (int d : f.decades)
                if (g.releaseYear >= d && g.releaseYear <= d + 9) { ok = true; break; }
        if (!ok) return false;
    }
    if (!f.completions.isEmpty())
    {
        bool ok = false;
        for (int c : f.completions)
            if (g.completion == c) { ok = true; break; }
        if (!ok) return false;
    }
    if (!triOk(f.favorite, g.favorite)) return false;
    if (!triOk(f.played, g.playSeconds > 0)) return false;
    if (!triOk(f.hidden, g.hidden)) return false;
    return true;
}

// ---- JSON round-trip ------------------------------------------------------------------------------------
// Only non-default dimensions are written, so a preset's stored blob names exactly the constraints the user
// chose (and toJson()/fromJson() are inverse over any Filter). Tri fields serialize as "yes"/"no" and are
// simply absent when Any.
static QJsonArray strArr(const QStringList& xs)
{
    QJsonArray a;
    for (const QString& x : xs) a.append(x);
    return a;
}
static QJsonArray intArr(const QVector<int>& xs)
{
    QJsonArray a;
    for (int x : xs) a.append(x);
    return a;
}
static QString triStr(Tri t) { return t == Tri::Yes ? QStringLiteral("yes") : QStringLiteral("no"); }

QJsonObject Filter::toJson() const
{
    QJsonObject o;
    if (!systems.isEmpty())     o.insert(QStringLiteral("systems"), strArr(systems));
    if (!tags.isEmpty())        o.insert(QStringLiteral("tags"), strArr(tags));
    if (!genres.isEmpty())      o.insert(QStringLiteral("genres"), strArr(genres));
    if (!minPlayers.isEmpty())  o.insert(QStringLiteral("minPlayers"), intArr(minPlayers));
    if (!decades.isEmpty())     o.insert(QStringLiteral("decades"), intArr(decades));
    if (!completions.isEmpty()) o.insert(QStringLiteral("completions"), intArr(completions));
    if (favorite != Tri::Any)   o.insert(QStringLiteral("favorite"), triStr(favorite));
    if (played != Tri::Any)     o.insert(QStringLiteral("played"), triStr(played));
    if (hidden != Tri::Any)     o.insert(QStringLiteral("hidden"), triStr(hidden));
    return o;
}

static QStringList readStrArr(const QJsonValue& v)
{
    QStringList out;
    for (const QJsonValue& e : v.toArray())
        if (e.isString() && !e.toString().isEmpty()) out << e.toString();
    return out;
}
static QVector<int> readIntArr(const QJsonValue& v)
{
    QVector<int> out;
    for (const QJsonValue& e : v.toArray())
        if (e.isDouble()) out << e.toInt();
    return out;
}
static Tri readTri(const QJsonValue& v)
{
    const QString s = v.toString();
    if (s == QStringLiteral("yes")) return Tri::Yes;
    if (s == QStringLiteral("no"))  return Tri::No;
    return Tri::Any;
}

Filter Filter::fromJson(const QJsonObject& o)
{
    Filter f;
    f.systems     = readStrArr(o.value(QStringLiteral("systems")));
    f.tags        = readStrArr(o.value(QStringLiteral("tags")));
    f.genres      = readStrArr(o.value(QStringLiteral("genres")));
    f.minPlayers  = readIntArr(o.value(QStringLiteral("minPlayers")));
    f.decades     = readIntArr(o.value(QStringLiteral("decades")));
    f.completions = readIntArr(o.value(QStringLiteral("completions")));
    f.favorite    = readTri(o.value(QStringLiteral("favorite")));
    f.played      = readTri(o.value(QStringLiteral("played")));
    f.hidden      = readTri(o.value(QStringLiteral("hidden")));
    return f;
}

// ---- Human summary --------------------------------------------------------------------------------------
QString Filter::describe() const
{
    if (isEmpty()) return QStringLiteral("All games");
    QStringList parts;
    if (!systems.isEmpty()) parts << systems.join(QStringLiteral("/")).toUpper();
    if (favorite == Tri::Yes) parts << QStringLiteral("★ Favorites");
    else if (favorite == Tri::No) parts << QStringLiteral("Not favorite");
    if (played == Tri::Yes) parts << QStringLiteral("Played");
    else if (played == Tri::No) parts << QStringLiteral("Unplayed");
    if (hidden == Tri::Yes) parts << QStringLiteral("Hidden");
    if (!tags.isEmpty()) parts << tags.join(QStringLiteral("/"));
    if (!genres.isEmpty()) parts << genres.join(QStringLiteral("/"));
    if (!minPlayers.isEmpty())
    {
        int lo = minPlayers.first();
        for (int n : minPlayers) lo = qMin(lo, n);   // ">= smallest threshold" is what the OR-set means
        parts << QStringLiteral("%1P+").arg(lo);
    }
    if (!decades.isEmpty())
    {
        QStringList ds;
        for (int d : decades) ds << QStringLiteral("%1s").arg(d);
        parts << ds.join(QStringLiteral("/"));
    }
    if (!completions.isEmpty())
    {
        // 0 None, 1 InProgress, 2 Finished, 3 Abandoned, 4 Planned — mirrors ItemMarks::Completion.
        static const char* kNames[] = { "None", "In progress", "Finished", "Abandoned", "Planned" };
        QStringList cs;
        for (int c : completions) if (c >= 0 && c <= 4) cs << QString::fromLatin1(kNames[c]);
        if (!cs.isEmpty()) parts << cs.join(QStringLiteral("/"));
    }
    return parts.join(QStringLiteral(" · "));
}

// ---- Scraped-field parsers ------------------------------------------------------------------------------
int parseMaxPlayers(const QString& raw)
{
    // Take the largest run of digits: "1-4" -> 4, "Up to 8" -> 8, "2 Players" -> 2, "1" -> 1.
    int best = 0;
    static const QRegularExpression re(QStringLiteral("\\d+"));
    auto it = re.globalMatch(raw);
    while (it.hasNext())
    {
        const int n = it.next().captured().toInt();
        if (n > best) best = n;
    }
    return best;
}

QStringList splitGenres(const QString& raw)
{
    QStringList out;
    static const QRegularExpression sep(QStringLiteral("[/,;\\x{2022}|]"));
    for (QString g : raw.split(sep, Qt::SkipEmptyParts))
    {
        g = g.trimmed();
        if (!g.isEmpty()) out << g;
    }
    return out;
}

int parseYear(const QString& raw)
{
    // The first four consecutive digits that name a plausible year. Covers "1996", ES "19960101T000000",
    // and "1996-03-21"; rejects a bare "12" or a 3-digit id.
    static const QRegularExpression re(QStringLiteral("(\\d{4})"));
    const auto m = re.match(raw);
    if (!m.hasMatch()) return 0;
    const int y = m.captured(1).toInt();
    return (y >= 1950 && y <= 2100) ? y : 0;
}
} // namespace gamefilter
