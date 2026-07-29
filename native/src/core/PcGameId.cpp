#include "PcGameId.h"

#include "AppBrand.h"
#include "AppPaths.h"

#include <QFileInfo>
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
//
// ANCHORED TO THE SUFFIX (`\s*$`), and applied repeatedly rather than globally. Edition noise is a
// thing stores APPEND; the same words in the middle of a title are part of the product name. Measured
// on the unanchored form: "Command & Conquer Remastered Collection" lost its "Remastered" and became
// "command conquer collection", colliding with the genuinely different "Command & Conquer Collection"
// — a merge, which is the direction that costs the user a game. The loop is what still handles noise
// STACKED on the end ("... Definitive Edition Remastered"): each pass peels the last phrase off.
const QRegularExpression& editionSuffixRe()
{
    static const QRegularExpression re(
        QStringLiteral("\\s*\\b(game of the year edition|game of the year|definitive edition"
                       "|complete edition|enhanced edition|director's cut|remastered|goty)\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// A leftover trailing article, dropped ONLY when an edition phrase was actually stripped off the end.
// "Portal 2: The Definitive Edition Remastered" peels down to "Portal 2: The", and that dangling "The"
// belonged to the noise, not to the game. Conditioning it on a real strip is what keeps it away from
// titles that genuinely end in an article; `^|\s` so a title that was NOTHING but "The <noise>"
// collapses to empty (and then gets a private mergeKey) instead of bucketing under "the".
const QRegularExpression& trailingArticleRe()
{
    static const QRegularExpression re(QStringLiteral("(?:^|\\s)(?:the|a|an)$"));
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

// KNOWN, DELIBERATE COLLISION — a remake normalises onto its original. The trailing-year strip is what
// makes "Prey (2017)" merge with the "Prey" a store lists bare, and it cannot tell that apart from
// "Prey (2017)" vs the 2006 game: both sides land on "prey", as do "Resident Evil 2" and "Resident
// Evil 2 (2019)". The year is precisely the thing stores use to disambiguate, and stripping it is a
// hard requirement (most catalogues carry the year on ONE of the two copies, so keeping it would split
// every ordinary game in two) — so the collision is priced in, not a bug to fix here.
//
// The igdb id is what actually separates them: sameGame() returns false for two different ids even
// when the titles agree, and mergeKey() keys on the id whenever there is one. The consequence for
// callers, and the reason this note exists: a TITLE-ONLY library — no metadata provider, or entries
// the provider did not resolve — will show a remake and its original as ONE entry. That is not
// recoverable inside this function; the cures are an igdb id or the user override store.
QString pcgame::normalizeTitle(const QString& raw)
{
    QString s = raw;

    // 1. Trademark / registered / copyright marks ("BioShock™ Remastered"). Done before the phrase
    //    strip so a mark glued to a phrase boundary cannot block the \b match.
    s.remove(QChar(0x2122)).remove(QChar(0x00AE)).remove(QChar(0x00A9));
    // Typographic apostrophes, so "Director’s Cut" matches the same phrase as "Director's Cut".
    s.replace(QChar(0x2019), QLatin1Char('\''));

    // 2. A trailing parenthesised year. See the note above this function: this is what merges a remake
    //    with its original, and the igdb id is what tells them apart again.
    s.remove(trailingYearRe());

    // 3. Edition noise — explicit phrases, at the END only, peeled one at a time. See editionSuffixRe().
    bool strippedEdition = false;
    for (int pass = 0; pass < 8; ++pass)  // bounded: the list is finite and each pass shortens s
    {
        const QRegularExpressionMatch m = editionSuffixRe().match(s);
        if (!m.hasMatch()) break;
        s.truncate(m.capturedStart(0));
        strippedEdition = true;
    }

    // 4. Remaining punctuation becomes a space (not nothing), so "Diablo II:Resurrected" does not fuse
    //    into one token.
    s.replace(punctRe(), QStringLiteral(" "));

    // 5. Collapse and case-fold. Numerals — "2", "II", "V", "VI" — survive all of the above untouched,
    //    which is the whole point of this function.
    s = s.replace(wsRe(), QStringLiteral(" ")).trimmed().toLower();

    // 6. The article the stripped phrase left behind ("Portal 2: The" -> "portal 2"). Only after a real
    //    strip. See trailingArticleRe().
    if (strippedEdition) s = s.remove(trailingArticleRe()).trimmed();
    return s;
}

QString pcgame::mergeKey(const QString& title, const QString& igdbId)
{
    if (!igdbId.isEmpty()) return igdbId;

    const QString norm = normalizeTitle(title);
    if (!norm.isEmpty()) return norm;

    // The empty-key guard, and it is load-bearing. A title can normalise to NOTHING — "!!!" is all
    // punctuation, "GOTY" and "Enhanced Edition" are all edition noise — and grouping is defined as
    // "equal mergeKey", so returning "" here would drop every such id-less entry into ONE bucket and
    // the catalog builder would fuse unrelated games into a single tile. sameGame() already refuses to
    // match two empty titles; this is the same rule, stated where grouping actually reads it, so no
    // caller has to know about the case.
    //
    // The fallback is derived from the RAW title rather than being unique-per-call on purpose: it must
    // be STABLE (mergeKey is a pure key, re-derived per scan, so a counter or a random value would make
    // an entry stop matching itself) while still separating two different noise titles. The prefix
    // carries ':' and '/', neither of which normalizeTitle can ever emit — it strips all punctuation —
    // so a fallback key cannot collide with a real title key, and the namespace keeps it clear of a
    // provider id. A title that is genuinely empty has nothing to derive from and keys on the bare
    // prefix; there is no information here to separate two nameless entries with.
    return QStringLiteral("pcgame:rawtitle/") + title.simplified().toLower();
}

QString pcgame::itemId(const QString& title)
{
    const QString t = title.trimmed();
    // Nothing to group on. Returning the bare "pcgame:rawtitle/" fallback here would hand EVERY nameless
    // entry the same id and fuse unrelated games into one tile / one record, so an empty title has no id
    // at all and both callers drop the entry instead.
    if (t.isEmpty()) return QString();

    const QString key = mergeKey(t, QString());   // title-only by decision — see the header
    if (key.isEmpty()) return QString();           // defensive: mergeKey's own guard means this cannot fire

    // mergeKey already returns a NAMESPACED key ("pcgame:rawtitle/…") for a title that normalises to
    // nothing, so prefixing unconditionally would produce "pcgame:pcgame:rawtitle/…". A normalised title
    // can never contain ':' (normalizeTitle strips all punctuation), so this test is exact, not a guess.
    return key.startsWith(QStringLiteral("pcgame:")) ? key : (QStringLiteral("pcgame:") + key);
}

// See the header for WHY this is a function and not two inline ternaries. It is deliberately total and
// pure: no filesystem access (QFileInfo only splits the string here), so the catalog and the remap get the
// same answer for a path that no longer exists — which is exactly the record most in need of migrating.
QString pcgame::downloadedTitle(const QString& title, const QString& path)
{
    return title.trimmed().isEmpty() ? QFileInfo(path).completeBaseName() : title;
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

QString pcgame::legacyLaunchId(const PcGameSource& s)
{
    if (s.launcher.isEmpty()) return QString();
    // The launcher's own id when it has one; otherwise its own NAME for this copy. The fallback is the
    // whole point (see the header): a code-less Battle.net title has no product code, so the id the old
    // battleNetGamesCatalog minted — and the candidate populatePcGames feeds remapTable — is its name.
    // The merged tile's display title is emphatically NOT interchangeable with it.
    const QString key = s.launchId.isEmpty() ? s.sourceName : s.launchId;
    if (key.isEmpty()) return QString();
    if (s.launcher == QStringLiteral("steam"))     return QStringLiteral("steam:") + key;
    if (s.launcher == QStringLiteral("epic"))      return QStringLiteral("epic:")  + key;
    if (s.launcher == QStringLiteral("gog"))       return QStringLiteral("gog:")   + key;
    if (s.launcher == QStringLiteral("battlenet")) return QStringLiteral("bnet:")  + key;
    return QString();   // a launcher with no id scheme here: no pre-merge id to claim (rule 1)
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
