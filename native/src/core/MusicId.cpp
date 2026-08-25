#include "MusicId.h"

#include "AppBrand.h"
#include "AppPaths.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

// ------------------------------------------------------------------------------------------------------
// The store. AppPaths/AppBrand are header-only, so this translation unit stays QtCore-only and
// probe_musicid links against Qt6::Core alone.
// ------------------------------------------------------------------------------------------------------
#ifdef EB_MUSICID_TEST_SEAM
QString    g_testIniPath;
QSettings* g_testStore = nullptr;
#endif

QSettings& store()
{
#ifdef EB_MUSICID_TEST_SEAM
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

const QString kAlbumGroup  = QStringLiteral("music/albumalias");
const QString kArtistGroup = QStringLiteral("music/artistalias");

// The pair key, canonical by construction: the two stored keys SORTED. Symmetry is a property of the key
// rather than of a second lookup somebody can forget to write. '|' is the separator because every stored key
// has had its punctuation removed, so it can never occur inside one.
QString pairKey(const QString& group, const QString& a, const QString& b)
{
    QStringList pair{ a, b };
    pair.sort();
    return group + QStringLiteral("/") + pair.at(0) + QStringLiteral("|") + pair.at(1);
}

// Every verdict, cached per group. The merge builder asks about a great many pairs in one pass — a library
// of five hundred artists against a server of five hundred is a lot of lookups — and re-reading the ini group
// each time is the per-item cost this folder has already been bitten by. Read once, held, and dropped by
// EVERY writer (including the test seam, or a probe that re-points the ini would read the old file's list
// and do it silently).
struct VerdictCache
{
    QHash<QString, bool>          byPair;    // "a|b" (sorted) -> same
    QVector<MusicId::Verdict>     list;
    bool                          loaded = false;
};

VerdictCache g_albumCache;
VerdictCache g_artistCache;

void invalidate()
{
    g_albumCache  = VerdictCache{};
    g_artistCache = VerdictCache{};
}

VerdictCache& cacheFor(const QString& group)
{
    VerdictCache& c = (group == kAlbumGroup) ? g_albumCache : g_artistCache;
    if (c.loaded) return c;
    c.loaded = true;
    QSettings& s = store();
    s.beginGroup(group);
    const QStringList keys = s.childKeys();
    for (const QString& k : keys)
    {
        const int bar = k.indexOf(QLatin1Char('|'));
        if (bar <= 0) continue;
        MusicId::Verdict v;
        v.a    = k.left(bar);
        v.b    = k.mid(bar + 1);
        v.same = s.value(k).toBool();
        if (v.b.isEmpty()) continue;
        c.list.push_back(v);
        c.byPair.insert(k, v.same);
    }
    s.endGroup();
    return c;
}

// -1 nothing said, 0 "not the same", 1 "the same". STORED KEYS, verbatim.
int verdictForKeys(const QString& group, const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty()) return -1;
    QStringList pair{ a, b };
    pair.sort();
    const QString k = pair.at(0) + QStringLiteral("|") + pair.at(1);
    const VerdictCache& c = cacheFor(group);
    const auto it = c.byPair.constFind(k);
    return it == c.byPair.constEnd() ? -1 : (*it ? 1 : 0);
}

void writeVerdict(const QString& group, const QString& a, const QString& b, bool same)
{
    if (a.isEmpty() || b.isEmpty()) return;
    store().setValue(pairKey(group, a, b), same);
    invalidate();
}

void eraseVerdict(const QString& group, const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty()) return;
    store().remove(pairKey(group, a, b));
    invalidate();
}

// ------------------------------------------------------------------------------------------------------
// Text plumbing
// ------------------------------------------------------------------------------------------------------

const QRegularExpression& punctRe()
{
    // Everything that is neither a letter, a digit nor whitespace. Unicode classes rather than [a-z0-9], so
    // a Cyrillic or CJK title keeps its letters instead of dissolving into nothing.
    static const QRegularExpression re(QStringLiteral("[^\\p{L}\\p{N}\\s]"));
    return re;
}

const QRegularExpression& wsRe()
{
    static const QRegularExpression re(QStringLiteral("\\s+"));
    return re;
}

// A bracketed group of any of the three flavours, with no nesting. Applied repeatedly rather than globally so
// that dropping one group cannot change what the next one matches.
const QRegularExpression& bracketRe()
{
    static const QRegularExpression re(QStringLiteral("[\\(\\[\\{]([^\\(\\)\\[\\]\\{\\}]*)[\\)\\]\\}]"));
    return re;
}

QString stripMarksAndFold(const QString& raw)
{
    QString s = raw;
    // Trademark / registered / copyright, and the typographic apostrophe, folded first so that a mark glued
    // to a word boundary cannot block a later whole-word match.
    s.remove(QChar(0x2122)).remove(QChar(0x00AE)).remove(QChar(0x00A9));
    s.replace(QChar(0x2019), QLatin1Char('\''));
    // THE APOSTROPHE IS REMOVED, not turned into a space like every other punctuation mark. It is the one
    // punctuation mark that sits INSIDE a word: "Sgt. Pepper's" would otherwise tokenise to "sgt pepper s"
    // while the same record tagged "Sgt Peppers" gives "sgt peppers", and the two would never match.
    // Removing it makes both "peppers", and does the same for "Don't"/"Dont" and "Rock 'n' Roll".
    s.remove(QLatin1Char('\''));
    // "&" is read as "and" BEFORE punctuation is removed, because it is punctuation. "Simon & Garfunkel" and
    // "Simon and Garfunkel" are the same act spelled two ways, and every tagger picks a different one.
    s.replace(QLatin1Char('&'), QStringLiteral(" and "));
    s = s.toCaseFolded();
    // Diacritics: decompose, then drop the combining marks. "Beyoncé" -> "beyonce", "Mötley Crüe" ->
    // "motley crue" — the two spellings a library and a server routinely disagree on.
    const QString d = s.normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(d.size());
    for (const QChar c : d)
        if (c.category() != QChar::Mark_NonSpacing) out.append(c);
    return out;
}

// RUNS OF SINGLE LETTERS ARE ONE WORD. Punctuation has just become whitespace, so "U.S.A." arrives as three
// tokens while the same title tagged "USA" arrives as one — and "R.E.M.", "M.I.A." and "L.A. Woman" are the
// same problem in an artist name. A run of TWO OR MORE adjacent single-character tokens is therefore glued
// back together.
//
// ONLY SINGLE-CHARACTER TOKENS JOIN A RUN, and a run of one is left exactly as it was. That is what keeps a
// lone single letter an ordinary word — "Kid A", "In a Silent Way", the "n" of "Guns N Roses" — rather than
// something glued to its neighbour, which would fuse titles that differ by a real word. (The `>= 2` test
// below is the same answer as `>= 1`, since joining a one-token run yields that token; it is written this way
// to say which case the branch is for. What must NOT change is the run predicate on the line above it.)
QStringList collapseAcronyms(const QStringList& toks)
{
    QStringList out;
    int i = 0;
    while (i < toks.size())
    {
        int j = i;
        while (j < toks.size() && toks.at(j).size() == 1) ++j;
        if (j - i >= 2) { out << toks.mid(i, j - i).join(QString()); i = j; }
        else            { out << toks.at(i); ++i; }
    }
    return out;
}

QStringList tokenize(const QString& s)
{
    QString t = s;
    t.replace(punctRe(), QStringLiteral(" "));
    t.replace(wsRe(), QStringLiteral(" "));
    return t.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

// ---- Edition noise -----------------------------------------------------------------------------------
//
// A group of tokens is DROPPABLE when every token in it is noise-compatible AND at least one of them is a
// STRONG noise word. The strong requirement is the whole safety property: without it "(Live)", "(Mono)",
// "(2009)" and "(Demo)" would all be droppable groups of weak/neutral tokens, and each of those can be the
// entire difference between two records the user owns separately.
const QSet<QString>& strongNoise()
{
    static const QSet<QString> s = {
        QStringLiteral("remaster"),  QStringLiteral("remastered"), QStringLiteral("remasters"),
        QStringLiteral("remastering"), QStringLiteral("deluxe"),   QStringLiteral("edition"),
        QStringLiteral("editions"),  QStringLiteral("reissue"),    QStringLiteral("reissued"),
        QStringLiteral("expanded"),  QStringLiteral("anniversary"),QStringLiteral("explicit"),
        QStringLiteral("bonus")
    };
    return s;
}

// Tokens allowed to keep a droppable group company. None of them can trigger a drop on its own.
const QSet<QString>& weakNoise()
{
    static const QSet<QString> s = {
        QStringLiteral("version"),  QStringLiteral("versions"),  QStringLiteral("special"),
        QStringLiteral("limited"),  QStringLiteral("collector"), QStringLiteral("collectors"),
        QStringLiteral("track"),    QStringLiteral("tracks"),    QStringLiteral("original"),
        QStringLiteral("originals"),QStringLiteral("recording"), QStringLiteral("recordings"),
        QStringLiteral("digital"),  QStringLiteral("digitally"), QStringLiteral("extended"),
        QStringLiteral("the"),      QStringLiteral("of"),        QStringLiteral("a"),
        QStringLiteral("an"),       QStringLiteral("and"),       QStringLiteral("plus")
    };
    return s;
}

bool isYearToken(const QString& t)
{
    if (t.size() != 4) return false;
    for (const QChar c : t) if (!c.isDigit()) return false;
    return true;
}

bool isOrdinalToken(const QString& t)
{
    // "20th", "1st", "2nd", "3rd" — what an anniversary edition is always spelled with, and a token that
    // would otherwise keep "(20th Anniversary Edition)" from being droppable at all.
    static const QRegularExpression re(QStringLiteral("^\\d+(st|nd|rd|th)$"));
    return re.match(t).hasMatch();
}

bool noiseCompatible(const QString& t)
{
    return strongNoise().contains(t) || weakNoise().contains(t) || isYearToken(t) || isOrdinalToken(t);
}

bool droppableRun(const QStringList& toks)
{
    if (toks.isEmpty()) return false;
    bool strong = false;
    for (const QString& t : toks)
    {
        if (!noiseCompatible(t)) return false;
        if (strongNoise().contains(t)) strong = true;
    }
    return strong;
}

// Drop every bracketed group whose contents are a droppable run. Loops rather than running globally in one
// pass so that removing one group re-anchors the search for the next.
QString dropNoiseBrackets(const QString& in)
{
    QString s = in;
    for (int guard = 0; guard < 8; ++guard)
    {
        const QRegularExpressionMatch m = bracketRe().match(s);
        if (!m.hasMatch()) break;
        bool removedAny = false;
        int  from = 0;
        while (true)
        {
            const QRegularExpressionMatch mm = bracketRe().match(s, from);
            if (!mm.hasMatch()) break;
            if (droppableRun(tokenize(mm.captured(1))))
            {
                s.remove(mm.capturedStart(0), mm.capturedLength(0));
                removedAny = true;
                from = 0;
            }
            else
            {
                from = mm.capturedEnd(0);
            }
        }
        if (!removedAny) break;
    }
    return s;
}

// Drop a trailing " - Deluxe Edition" / " : 2009 Remaster" segment. The separator forms below are what the
// stores and taggers actually write; the test is the same droppable-run rule, so a trailing " - Live at
// Wembley" survives untouched.
QString dropNoiseTail(const QString& in)
{
    static const QStringList seps = {
        QStringLiteral(" - "), QString(QChar(0x2013)).prepend(QLatin1Char(' ')).append(QLatin1Char(' ')),
        QString(QChar(0x2014)).prepend(QLatin1Char(' ')).append(QLatin1Char(' ')),
        QStringLiteral(": "), QStringLiteral(" / ")
    };
    QString s = in;
    for (int guard = 0; guard < 4; ++guard)
    {
        int    cut = -1;
        int    cutLen = 0;
        for (const QString& sep : seps)
        {
            const int at = s.lastIndexOf(sep);
            if (at > cut) { cut = at; cutLen = sep.size(); }
        }
        if (cut <= 0) break;
        if (!droppableRun(tokenize(s.mid(cut + cutLen)))) break;
        s = s.left(cut);
    }
    return s;
}

// ---- Sequel words and numerals -----------------------------------------------------------------------

// The canonical spelling of a sequel word. "Vol.", "Vol" and "Volume" are one word written three ways, and
// a library and a server will disagree about which one within the same album's tags.
QString canonicalSequelWord(const QString& t)
{
    if (t == QLatin1String("volume")) return QStringLiteral("vol");
    if (t == QLatin1String("part"))   return QStringLiteral("pt");
    if (t == QLatin1String("number")) return QStringLiteral("no");
    if (t == QLatin1String("nr"))     return QStringLiteral("no");
    return t;
}

bool isSequelWord(const QString& canonical)
{
    static const QSet<QString> s = {
        QStringLiteral("vol"), QStringLiteral("pt"), QStringLiteral("no"), QStringLiteral("chapter"),
        QStringLiteral("book"), QStringLiteral("act"), QStringLiteral("disc"), QStringLiteral("episode"),
        QStringLiteral("season")
    };
    return s.contains(canonical);
}

// The ONE bound that keeps real words out of the levelling. See MusicId.h.
const int kMaxLevelledRoman = 30;

} // namespace

// ==========================================================================================================

#ifdef EB_MUSICID_TEST_SEAM
void MusicId::setIniPathForTesting(const QString& path)
{
    delete g_testStore;
    g_testStore   = nullptr;
    g_testIniPath = path;
    invalidate();   // the cached verdicts came from the OLD file
}
#endif

int MusicId::romanValue(const QString& token)
{
    // Strict subtractive form only: "iiii" and "vx" are not numerals, and accepting them would widen the set
    // of ordinary words that level.
    static const QRegularExpression re(
        QStringLiteral("^m{0,3}(cm|cd|d?c{0,3})(xc|xl|l?x{0,3})(ix|iv|v?i{0,3})$"));
    const QString t = token.toCaseFolded();
    if (t.isEmpty()) return 0;
    if (!re.match(t).hasMatch()) return 0;

    int total = 0, prev = 0;
    for (int i = t.size() - 1; i >= 0; --i)
    {
        int v = 0;
        switch (t.at(i).unicode())
        {
        case 'i': v = 1;    break;
        case 'v': v = 5;    break;
        case 'x': v = 10;   break;
        case 'l': v = 50;   break;
        case 'c': v = 100;  break;
        case 'd': v = 500;  break;
        case 'm': v = 1000; break;
        default:  return 0;
        }
        total += (v < prev) ? -v : v;
        prev = std::max(prev, v);
    }
    return total;
}

QString MusicId::normalizeArtist(const QString& raw)
{
    QStringList toks = tokenize(stripMarksAndFold(raw));

    // The featured-artist tail. One rule covers both spellings a tagger uses — "Jay-Z feat. Alicia Keys" and
    // "Jay-Z (feat. Alicia Keys)" — because the brackets have already become spaces by the time we look.
    // Never at index 0: an act whose name STARTS with one of these words is not a credit, it is a name.
    for (int i = 1; i < toks.size(); ++i)
    {
        const QString& t = toks.at(i);
        if (t == QLatin1String("feat") || t == QLatin1String("ft") || t == QLatin1String("featuring"))
        {
            toks = toks.mid(0, i);
            break;
        }
    }

    // A leading article, dropped — but never to nothing. "The Beatles" and "Beatles" are one act; an act
    // called "The" keeps its one token rather than bucketing with every other unnameable name.
    if (toks.size() > 1)
    {
        const QString& f = toks.first();
        if (f == QLatin1String("the") || f == QLatin1String("a") || f == QLatin1String("an"))
            toks.removeFirst();
    }

    return collapseAcronyms(toks).join(QLatin1Char(' '));
}

QString MusicId::normalizeAlbum(const QString& raw)
{
    // Steps 1-2: marks, "&", case, diacritics — then the edition strips, which run while the brackets and
    // separators are still there because that is what they are anchored to.
    QString s = stripMarksAndFold(raw);
    s = dropNoiseBrackets(s);
    s = dropNoiseTail(s);

    QStringList toks = tokenize(s);

    // A bare trailing noise run ("Album Deluxe Edition", "Album Original Recording Remastered"). Peeled only
    // when the peeled run carries a strong word AND something survives it: an album genuinely CALLED "Bonus"
    // must not normalise to nothing, and "Christmas Special" must not lose its "Special" to a rule meant for
    // "Special Edition".
    {
        int keep = toks.size();
        while (keep > 0 && noiseCompatible(toks.at(keep - 1))) --keep;
        if (keep > 0 && keep < toks.size() && droppableRun(toks.mid(keep)))
            toks = toks.mid(0, keep);
    }

    toks = collapseAcronyms(toks);
    for (QString& t : toks) t = canonicalSequelWord(t);

    // Roman levelling, on BOTH sides by construction (this is the only place either side is built). A token
    // levels when it is a numeral of value 1..30 AND either follows a sequel word or is a trailing token of
    // at least two characters. Never at index 0, so a one-word title called "I" or "X" is left alone.
    for (int i = 1; i < toks.size(); ++i)
    {
        const int v = romanValue(toks.at(i));
        if (v <= 0 || v > kMaxLevelledRoman) continue;
        const bool afterSequelWord = isSequelWord(toks.at(i - 1));
        const bool trailingNumeral = (i == toks.size() - 1) && toks.at(i).size() >= 2;
        if (afterSequelWord || trailingNumeral)
            toks[i] = QString::number(v);
    }

    return toks.join(QLatin1Char(' '));
}

// ---- MusicBrainz ground truth ---------------------------------------------------------------------------

MusicId::Ground MusicId::groundArtist(const QString& mbidA, const QString& mbidB)
{
    const QString a = mbidA.trimmed().toCaseFolded();
    const QString b = mbidB.trimmed().toCaseFolded();
    if (a.isEmpty() || b.isEmpty()) return Ground::Silent;
    return a == b ? Ground::Same : Ground::Different;
}

MusicId::Ground MusicId::groundAlbum(const AlbumMbid& a, const AlbumMbid& b)
{
    // LIKE WITH LIKE, ALWAYS. A release-group id and a release id name different things, so a comparison
    // between them means nothing — and would mean "Different" every time, which is the expensive answer.
    const QString ag = a.releaseGroup.trimmed().toCaseFolded();
    const QString bg = b.releaseGroup.trimmed().toCaseFolded();
    if (!ag.isEmpty() && !bg.isEmpty()) return ag == bg ? Ground::Same : Ground::Different;

    const QString ar = a.release.trimmed().toCaseFolded();
    const QString br = b.release.trimmed().toCaseFolded();
    if (!ar.isEmpty() && !br.isEmpty()) return ar == br ? Ground::Same : Ground::Different;

    return Ground::Silent;
}

// ---- Confidence -----------------------------------------------------------------------------------------

MusicId::Confidence MusicId::artistConfidence(const QString& nameA, const QString& mbidA,
                                              const QString& nameB, const QString& mbidB)
{
    const QString na = normalizeArtist(nameA);
    const QString nb = normalizeArtist(nameB);

    // 1. The user's own verdict, which beats everything including an MBID: they are looking at the two rows
    //    and the app is not.
    const int said = verdictForKeys(kArtistGroup, na, nb);
    if (said == 1) return Confidence::Certain;
    if (said == 0) return Confidence::TooLowToMerge;

    // 2. Ground truth, both ways.
    switch (groundArtist(mbidA, mbidB))
    {
    case Ground::Same:      return Confidence::Certain;
    case Ground::Different: return Confidence::TooLowToMerge;
    case Ground::Silent:    break;
    }

    // 3. The names. An unnameable artist matches nothing — including another unnameable artist, because two
    //    sources' untagged piles are two different piles and fusing them is the largest wrong merge available.
    if (na.isEmpty() || nb.isEmpty()) return Confidence::TooLowToMerge;
    if (na == nb) return Confidence::Likely;

    return Confidence::TooLowToMerge;
}

MusicId::Confidence MusicId::albumConfidence(const AlbumFacts& a, const AlbumFacts& b)
{
    const QString ka = albumKeyOf(a.albumArtist, a.title);
    const QString kb = albumKeyOf(b.albumArtist, b.title);

    const int said = verdictForKeys(kAlbumGroup, ka, kb);
    if (said == 1) return Confidence::Certain;
    if (said == 0) return Confidence::TooLowToMerge;

    switch (groundAlbum(a.mbid, b.mbid))
    {
    case Ground::Same:      return Confidence::Certain;
    case Ground::Different: return Confidence::TooLowToMerge;
    case Ground::Silent:    break;
    }

    if (!sameArtist(a.albumArtist, a.artistMbid, b.albumArtist, b.artistMbid))
        return Confidence::TooLowToMerge;

    const QString ta = normalizeAlbum(a.title);
    const QString tb = normalizeAlbum(b.title);
    if (ta.isEmpty() || tb.isEmpty()) return Confidence::TooLowToMerge;
    if (ta != tb) return Confidence::TooLowToMerge;

    // The year GATE. A disagreement of more than one year is positive evidence against — it is the only
    // thing that separates a live record from the studio album it is named after when neither title says so,
    // and it is what stops a 1973 album fusing with its 2009 remaster listing. One year of slack absorbs the
    // ordinary release-date drift between a rip and a server's own metadata. An unknown year is compatible
    // with everything: absence is not disagreement.
    if (a.year > 0 && b.year > 0 && std::abs(a.year - b.year) > 1)
        return Confidence::TooLowToMerge;

    return Confidence::Likely;
}

int MusicId::closeness(const AlbumFacts& a, const AlbumFacts& b)
{
    int dt = 0;
    if (a.trackCount > 0 && b.trackCount > 0) dt = std::abs(a.trackCount - b.trackCount);
    int dd = 0;
    if (a.durationSec > 0 && b.durationSec > 0) dd = std::min(std::abs(a.durationSec - b.durationSec), 99999);
    return dt * 100000 + dd;
}

// ---- Overrides ------------------------------------------------------------------------------------------

QString MusicId::albumKeyOf(const QString& rawArtist, const QString& rawTitle)
{
    const QString a = normalizeArtist(rawArtist);
    const QString t = normalizeAlbum(rawTitle);
    if (a.isEmpty() || t.isEmpty()) return QString();
    return a + QStringLiteral("!") + t;
}

void MusicId::setAlbumOverride(const QString& artistA, const QString& titleA,
                               const QString& artistB, const QString& titleB, bool same)
{ writeVerdict(kAlbumGroup, albumKeyOf(artistA, titleA), albumKeyOf(artistB, titleB), same); }

void MusicId::clearAlbumOverride(const QString& artistA, const QString& titleA,
                                 const QString& artistB, const QString& titleB)
{ eraseVerdict(kAlbumGroup, albumKeyOf(artistA, titleA), albumKeyOf(artistB, titleB)); }

int MusicId::albumOverrideVerdict(const QString& artistA, const QString& titleA,
                                  const QString& artistB, const QString& titleB)
{ return verdictForKeys(kAlbumGroup, albumKeyOf(artistA, titleA), albumKeyOf(artistB, titleB)); }

void MusicId::setArtistOverride(const QString& nameA, const QString& nameB, bool same)
{ writeVerdict(kArtistGroup, normalizeArtist(nameA), normalizeArtist(nameB), same); }

void MusicId::clearArtistOverride(const QString& nameA, const QString& nameB)
{ eraseVerdict(kArtistGroup, normalizeArtist(nameA), normalizeArtist(nameB)); }

int MusicId::artistOverrideVerdict(const QString& nameA, const QString& nameB)
{ return verdictForKeys(kArtistGroup, normalizeArtist(nameA), normalizeArtist(nameB)); }

QVector<MusicId::Verdict> MusicId::albumOverrides()  { return cacheFor(kAlbumGroup).list; }
QVector<MusicId::Verdict> MusicId::artistOverrides() { return cacheFor(kArtistGroup).list; }

void MusicId::clearAlbumOverrideKeys(const QString& keyA, const QString& keyB)
{ eraseVerdict(kAlbumGroup, keyA, keyB); }

void MusicId::clearArtistOverrideKeys(const QString& keyA, const QString& keyB)
{ eraseVerdict(kArtistGroup, keyA, keyB); }

// ---- Which copy plays -----------------------------------------------------------------------------------

int MusicId::pickAutoSource(const QVector<SourceRef>& all, const QString& preference)
{
    if (all.isEmpty()) return -1;

    const QString pref = preference.trimmed().isEmpty() ? QString::fromLatin1(kPreferLocal)
                                                        : preference.trimmed();

    // Rank, lowest wins. Availability first — a source that cannot answer right now must never be chosen to
    // play, whatever the preference says — then how well the instance matches the preference, then local
    // before remote, then the caller's own order (which is the order the servers were added). Total and
    // deterministic, so two identical libraries settle on the same copy and a merged row's identity does not
    // flap between refreshes.
    int best = -1, bestRank = 0;
    for (int i = 0; i < all.size(); ++i)
    {
        const SourceRef& s = all.at(i);
        const bool local = s.serverId.isEmpty();

        int prefRank = 2;
        if (pref == QLatin1String(kPreferLocal))       prefRank = local ? 0 : 1;
        else if (pref == QLatin1String(kPreferServer)) prefRank = local ? 1 : 0;
        else                                           prefRank = (s.serverId == pref) ? 0 : (local ? 1 : 2);

        const int rank = (s.available ? 0 : 1) * 1000 + prefRank * 100 + (local ? 0 : 10);
        if (best < 0 || rank < bestRank) { best = i; bestRank = rank; }
    }
    return best;
}
