#include "PcGameId.h"

#include "AppBrand.h"
#include "AppPaths.h"

#include <QRegularExpression>
#include <QSettings>
#include <QStringList>

namespace {

// Test-only redirect (see the header). Deleting the cached QSettings rather than only re-pointing a
// path is the load-bearing half: a function-local static QSettings is constructed exactly once, so a
// path captured on first use would pin every later case to the first one's file.
#ifdef EB_PCGAMEID_TEST_SEAM
QString    g_testIniPath;
QSettings* g_testStore = nullptr;
#endif

// The one ini the app already writes. AppPaths/AppBrand are header-only, so pulling them in keeps this
// translation unit QtCore-only and probe_pcgames links against Qt6::Core alone.
QSettings& store()
{
#ifdef EB_PCGAMEID_TEST_SEAM
    if (!g_testIniPath.isEmpty())
    {
        if (!g_testStore) g_testStore = new QSettings(g_testIniPath, QSettings::IniFormat);
        return *g_testStore;
    }
#endif
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Edition noise, matched as WHOLE PHRASES and nothing else. This list is the entire strip rule on
// purpose: the tempting generic form — "drop the trailing token(s) after the base title" — is exactly
// what eats "2" off "Portal 2" and "III" off "Diablo III", which merges two different games into one
// and DELETES one of them from the user's library. Adding noise here is cheap and reversible; a
// generic rule is neither. Longest-first so "game of the year edition" is not left as a stray
// "edition" by an earlier shorter match.
const QRegularExpression& editionRe()
{
    static const QRegularExpression re(
        QStringLiteral("\\b(game of the year edition|game of the year|definitive edition"
                       "|complete edition|enhanced edition|director's cut|remastered|goty)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// A trailing disambiguating year, as the stores write it: "Prey (2017)".
const QRegularExpression& trailingYearRe()
{
    static const QRegularExpression re(QStringLiteral("\\(\\s*\\d{4}\\s*\\)\\s*$"));
    return re;
}

// Everything that is neither a letter, a digit nor whitespace. Runs AFTER the phrase strips, because
// the phrases themselves carry punctuation ("director's cut"). Letters and digits are matched by
// Unicode class, not by [a-z0-9], so an accented title keeps its letters instead of dissolving.
const QRegularExpression& punctRe()
{
    static const QRegularExpression re(QStringLiteral("[^\\p{L}\\p{N}\\s]"));
    return re;
}

const QRegularExpression& wsRe()
{
    static const QRegularExpression re(QStringLiteral("\\s+"));
    return re;
}

// The pair key, canonical by construction: the two normalised titles SORTED. Symmetry is then a
// property of the key rather than of a second lookup somebody can forget to write.
QString pairKey(const QString& normA, const QString& normB)
{
    QStringList pair{ normA, normB };
    pair.sort();
    return QStringLiteral("pcgames/alias/") + pair.at(0) + QStringLiteral("|") + pair.at(1);
}

// -1 = the user has said nothing about this pair, 0 = "not the same", 1 = "the same".
int overrideValue(const QString& normA, const QString& normB)
{
    const QString a = pcgame::normalizeTitle(normA);
    const QString b = pcgame::normalizeTitle(normB);
    if (a.isEmpty() || b.isEmpty()) return -1;
    const QVariant v = store().value(pairKey(a, b));
    if (!v.isValid()) return -1;
    return v.toBool() ? 1 : 0;
}

} // namespace

#ifdef EB_PCGAMEID_TEST_SEAM
void pcgame::setIniPathForTesting(const QString& path)
{
    delete g_testStore;
    g_testStore   = nullptr;
    g_testIniPath = path;
}
#endif

QString pcgame::normalizeTitle(const QString& raw)
{
    QString s = raw;

    // 1. Trademark / registered / copyright marks ("BioShock™ Remastered"). Done before the phrase
    //    strip so a mark glued to a phrase boundary cannot block the \b match.
    s.remove(QChar(0x2122)).remove(QChar(0x00AE)).remove(QChar(0x00A9));
    // Typographic apostrophes, so "Director’s Cut" matches the same phrase as "Director's Cut".
    s.replace(QChar(0x2019), QLatin1Char('\''));

    // 2. A trailing parenthesised year.
    s.remove(trailingYearRe());

    // 3. Edition noise — explicit phrases only. See editionRe().
    s.remove(editionRe());

    // 4. Remaining punctuation becomes a space (not nothing), so "Diablo II:Resurrected" does not fuse
    //    into one token.
    s.replace(punctRe(), QStringLiteral(" "));

    // 5. Collapse and case-fold. Numerals — "2", "II", "V", "VI" — survive all of the above untouched,
    //    which is the whole point of this function.
    s = s.replace(wsRe(), QStringLiteral(" ")).trimmed().toLower();
    return s;
}

QString pcgame::mergeKey(const QString& title, const QString& igdbId)
{
    return igdbId.isEmpty() ? normalizeTitle(title) : igdbId;
}

bool pcgame::sameGame(const QString& titleA, const QString& igdbA,
                      const QString& titleB, const QString& igdbB)
{
    const QString na = normalizeTitle(titleA);
    const QString nb = normalizeTitle(titleB);

    // 1. The user's verdict, FIRST. It has to beat both heuristics below — including two matching igdb
    //    ids — or the escape hatch does not actually reach the cases people complain about.
    const int ov = overrideValue(na, nb);
    if (ov >= 0) return ov == 1;

    // 2. Ids decide only when BOTH sides have one. Two different ids mean NOT the same game even when
    //    the titles agree ("Prey" 2006 vs "Prey" 2017); a missing id on one side is not a mismatch, it
    //    just means there is nothing to compare, so fall through to the titles.
    if (!igdbA.isEmpty() && !igdbB.isEmpty()) return igdbA == igdbB;

    // 3. Titles.
    return !na.isEmpty() && na == nb;
}

bool pcgame::overrideSaysSame(const QString& normA, const QString& normB)
{
    return overrideValue(normA, normB) == 1;
}

void pcgame::setOverride(const QString& normA, const QString& normB, bool same)
{
    const QString a = normalizeTitle(normA);
    const QString b = normalizeTitle(normB);
    if (a.isEmpty() || b.isEmpty()) return;
    // A "not the same" verdict is STORED, not erased: it is the user correcting a wrong merge, and it
    // has to keep beating the heuristic on every later scan.
    store().setValue(pairKey(a, b), same);
    store().sync();
}

int pcgame::pickAutoSource(const QVector<PcGameSource>& all)
{
    int found = -1;
    for (int i = 0; i < all.size(); ++i)
    {
        if (!all.at(i).ready) continue;   // never, under any count, hand Play a source that downloads first
        if (found >= 0) return -1;        // several ready -> ask which one
        found = i;
    }
    return found;                          // the single ready one, or -1 when none is ready
}
