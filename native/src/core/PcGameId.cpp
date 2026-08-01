#include "PcGameId.h"

#include "AppBrand.h"
#include "AppPaths.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <algorithm>

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

// The ini GROUP every verdict lives in, and the separator inside the key. The separator is '|' because
// normalizeTitle strips all punctuation, so a normalised title can never contain one — the split back into
// two titles is therefore exact rather than a best guess. '/' would have been read by QSettings as a group
// separator, which is the other reason it is not used.
const QLatin1String kAliasGroup("pcgames/alias");

// The pair key, canonical by construction: the two normalised titles SORTED. Symmetry is then a
// property of the key rather than of a second lookup somebody can forget to write.
QString pairKey(const QString& normA, const QString& normB)
{
    QStringList pair{ normA, normB };
    pair.sort();
    return kAliasGroup + QStringLiteral("/") + pair.at(0) + QStringLiteral("|") + pair.at(1);
}

// Every verdict, cached. effectiveItemId is called once per library entry per refresh — a few hundred times
// for a real library — and each call needs the whole verdict set (a "same" verdict is a graph edge, so a
// point lookup cannot answer it). Re-enumerating the ini group that often is the kind of per-item cost this
// folder has already been bitten by, so the list is read once and held.
//
// EVERY writer invalidates it, including the test seam: a probe that re-points the ini and then read a list
// gathered from the previous file would be testing nothing, and would do it silently.
QVector<pcgame::MergeVerdict> g_verdicts;
bool                          g_verdictsLoaded = false;

void invalidateVerdicts() { g_verdictsLoaded = false; g_verdicts.clear(); }

// THE STORE'S ONE INVARIANT, and the whole reason this layer is split in two:
//
//   a stored key is normalizeTitle(a RAW title), applied EXACTLY ONCE.
//
// normalizeTitle is NOT idempotent, and cannot cheaply be made so. Step 3 (the edition-phrase strip) runs
// BEFORE step 4 (punctuation -> space) because the phrases themselves carry punctuation ("director's cut"),
// so a title whose edition phrase is wrapped in brackets is not a fixed point:
//
//   "Batman Arkham City (GOTY)" -> "batman arkham city goty" -> "batman arkham city"
//
// and the same for "(Remastered)", "(Definitive Edition)" or a trailing full stop. Bracketed edition
// suffixes are routine in downloaded release names, which is a population this folder explicitly ingests.
//
// So EVERY function that touches the store has to normalise its argument exactly once — never zero times
// (a raw title would be looked up under a key nothing writes) and never twice (a stored key would be
// re-normalised into a key nothing wrote, and the read/remove would silently miss). Two entry points make
// that countable rather than a convention:
//
//   verdictForKeys / clearOverrideKeys  take STORED KEYS, verbatim, and normalise nothing.
//   overrideValue / setOverride / clearOverride / overrideSaysSame / overrideSaysSeparate
//                                       take RAW TITLES and normalise once, here.
//
// The bug this replaces: the fuse path matched stored keys directly (correct) while the separate path went
// through a double normalisation, and Undo handed STORED keys back to clearOverride, which re-normalised
// them and removed a pair nobody had written — QSettings::remove no-ops, the toast said "Undone.", and the
// fuse survived every retry with the loser side's favourites and play time stranded for good.

// -1 = the user has said nothing about this pair, 0 = "not the same", 1 = "the same".
// STORED KEYS, verbatim: both sides must already be normalizeTitle output.
int verdictForKeys(const QString& keyA, const QString& keyB)
{
    if (keyA.isEmpty() || keyB.isEmpty()) return -1;
    const QVariant v = store().value(pairKey(keyA, keyB));
    if (!v.isValid()) return -1;
    return v.toBool() ? 1 : 0;
}

// The same question asked with RAW titles: normalise once, then ask.
int overrideValue(const QString& rawA, const QString& rawB)
{
    return verdictForKeys(pcgame::normalizeTitle(rawA), pcgame::normalizeTitle(rawB));
}

} // namespace

#ifdef EB_PCGAMEID_TEST_SEAM
void pcgame::setIniPathForTesting(const QString& path)
{
    delete g_testStore;
    g_testStore   = nullptr;
    g_testIniPath = path;
    invalidateVerdicts();   // the cached verdicts came from the OLD file
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
QString pcgame::normalizeTitle(const QString& raw) { return normalizeCore(raw, /*stripYear=*/true); }

// The whole of normalizeTitle, with the ONE step that the separation tag needs turned off. See
// pcgame::separationTag: keeping the year is exactly the difference a user is pointing at when they separate
// a wrongly fused key, and every OTHER step here removes edition noise, which must keep fusing.
QString pcgame::normalizeCore(const QString& raw, bool stripYear)
{
    QString s = raw;

    // 1. Trademark / registered / copyright marks ("BioShock™ Remastered"). Done before the phrase
    //    strip so a mark glued to a phrase boundary cannot block the \b match.
    s.remove(QChar(0x2122)).remove(QChar(0x00AE)).remove(QChar(0x00A9));
    // Typographic apostrophes, so "Director’s Cut" matches the same phrase as "Director's Cut".
    s.replace(QChar(0x2019), QLatin1Char('\''));

    // 2. A trailing parenthesised year. See the note above this function: this is what merges a remake
    //    with its original, and the igdb id is what tells them apart again.
    if (stripYear) s.remove(trailingYearRe());

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
    //
    //    verdictForKeys, not overrideValue: na/nb are ALREADY normalizeTitle output, and normalizeTitle is
    //    not a fixed point (see the note above verdictForKeys). Re-normalising here would ask about a key
    //    nothing ever wrote for exactly the titles most likely to need an override.
    const int ov = verdictForKeys(na, nb);
    if (ov >= 0) return ov == 1;

    // 2. Ids decide only when BOTH sides have one. Two different ids mean NOT the same game even when
    //    the titles agree ("Prey" 2006 vs "Prey" 2017); a missing id on one side is not a mismatch, it
    //    just means there is nothing to compare, so fall through to the titles.
    if (!igdbA.isEmpty() && !igdbB.isEmpty()) return igdbA == igdbB;

    // 3. Titles.
    return !na.isEmpty() && na == nb;
}

bool pcgame::overrideSaysSame(const QString& titleA, const QString& titleB)
{
    return overrideValue(titleA, titleB) == 1;
}

void pcgame::setOverride(const QString& titleA, const QString& titleB, bool same)
{
    const QString a = normalizeTitle(titleA);
    const QString b = normalizeTitle(titleB);
    if (a.isEmpty() || b.isEmpty()) return;
    // A "not the same" verdict is STORED, not erased: it is the user correcting a wrong merge, and it
    // has to keep beating the heuristic on every later scan.
    store().setValue(pairKey(a, b), same);
    store().sync();
    invalidateVerdicts();
}

void pcgame::clearOverride(const QString& titleA, const QString& titleB)
{
    clearOverrideKeys(normalizeTitle(titleA), normalizeTitle(titleB));
}

// The same removal named by the keys the verdict is ACTUALLY stored under — the form pcgame::overrides()
// hands back. Nothing is normalised here, and that is the point: the Undo surface walks the stored verdicts
// and clears them by name, so re-normalising would (for a title that is not a normalisation fixed point)
// build a key nobody wrote, remove nothing, and report success. See the note above verdictForKeys.
void pcgame::clearOverrideKeys(const QString& keyA, const QString& keyB)
{
    if (keyA.isEmpty() || keyB.isEmpty()) return;
    store().remove(pairKey(keyA, keyB));
    store().sync();
    invalidateVerdicts();
}

QVector<pcgame::MergeVerdict> pcgame::overrides()
{
    if (g_verdictsLoaded) return g_verdicts;

    QVector<MergeVerdict> out;
    QSettings& s = store();
    s.beginGroup(kAliasGroup);
    const QStringList keys = s.childKeys();
    for (const QString& k : keys)
    {
        // "<normA>|<normB>". indexOf, not section(): a normalised title cannot contain '|' (normalizeTitle
        // strips punctuation), so the FIRST separator is the only one and the split is exact. A key that
        // does not have this shape is not one of ours — skip it rather than inventing an empty side.
        const int cut = k.indexOf(QLatin1Char('|'));
        if (cut <= 0 || cut + 1 >= k.size()) continue;
        MergeVerdict v;
        v.a    = k.left(cut);
        v.b    = k.mid(cut + 1);
        v.same = s.value(k).toBool();
        out.push_back(v);
    }
    s.endGroup();

    g_verdicts       = out;
    g_verdictsLoaded = true;
    return g_verdicts;
}

QString pcgame::separationTag(const QString& title) { return normalizeCore(title, /*stripYear=*/false); }

bool pcgame::overrideSaysSeparate(const QString& title)
{
    // The SELF-pair. When the merge is the thing that is wrong, both titles normalise to the same key by
    // construction — that is WHY they fused — so there is no second key to name the verdict against, and
    // (norm, norm) is the only honest place to record "the copies under this key are not one game".
    //
    // ONE normalisation, and then a VERBATIM lookup. It used to normalise here and again inside
    // overrideValue, so for a title that is not a normalisation fixed point this asked about a key
    // setOverride had never written: the split's destructive confirm and success toast both fired and the
    // folder came back byte-identical. That the fuse branch of effectiveItemId matched stored keys directly
    // while this branch double-normalised is what made the two halves of one function disagree.
    const QString n = normalizeTitle(title);
    if (n.isEmpty()) return false;
    return verdictForKeys(n, n) == 0;
}

QStringList pcgame::fusedKeys(const QString& normKey)
{
    if (normKey.isEmpty()) return QStringList();
    const QVector<MergeVerdict> all = overrides();
    QSet<QString>               seen{ normKey };
    QStringList                 queue{ normKey };
    QStringList                 out;
    // Bounded by the number of stored verdicts: each iteration either enqueues a key never seen before or
    // ends. A user has a handful of these, not a graph. Order-independent and cycle-safe by the `seen` set,
    // so the component is the same whichever member you start from and whatever order the verdicts landed in.
    while (!queue.isEmpty())
    {
        const QString cur = queue.takeFirst();
        out << cur;
        for (const MergeVerdict& v : all)
        {
            if (!v.same) continue;
            QString other;
            if      (v.a == cur) other = v.b;
            else if (v.b == cur) other = v.a;
            else continue;
            if (other.isEmpty() || seen.contains(other)) continue;
            seen.insert(other);
            queue << other;
        }
    }
    return out;
}

// The smallest normalised key in the set of keys the user has fused together. Smallest-in-the-component
// rather than "whichever side the user pressed on" so the answer is the same from either entry and does not
// depend on the order the verdicts were recorded — an id that moved when a third alias was added would
// strand the records of the first two.
static QString canonicalNorm(const QString& norm)
{
    QString best = norm;
    for (const QString& k : pcgame::fusedKeys(norm))
        if (k < best) best = k;
    return best;
}

QString pcgame::fusedCanonicalKey(const QString& normA, const QString& normB)
{
    // Fusing A and B adds ONE edge, so the resulting component is exactly the union of the two existing
    // components — and the surviving key is its minimum. Computing it this way is the only way the confirm
    // can name the right survivor: the pairwise min of the two titles is wrong the moment either side was
    // already fused with something smaller, which is precisely when the user has most to lose.
    QString best = (normA.isEmpty() || (!normB.isEmpty() && normB < normA)) ? normB : normA;
    if (best.isEmpty()) return best;
    for (const QString& k : fusedKeys(normA)) if (k < best) best = k;
    for (const QString& k : fusedKeys(normB)) if (k < best) best = k;
    return best;
}

QString pcgame::effectiveItemId(const QString& title)
{
    const QString base = itemId(title);
    if (base.isEmpty()) return base;   // nothing to group on -> no id, override or not (rule 1)

    const QString norm = normalizeTitle(title);
    // A title that normalises to NOTHING is already keyed by mergeKey's private raw-title fallback, and the
    // verdict store is keyed on normalised titles — there is no key here for a verdict to be about. Returning
    // the base is not a shortcut; it is the only defined answer.
    if (norm.isEmpty()) return base;

    // SEPARATE first (see the header): a key cannot be both too coarse and too fine, and this branch is the
    // one that recovers a game the merge removed from the library, which is the worse direction.
    //
    // Handed the RAW title, not `norm`: overrideSaysSeparate normalises exactly once, so passing the
    // already-normalised key would be the second normalisation this whole layer exists to prevent.
    if (overrideSaysSeparate(title))
        return base + QStringLiteral("#") + separationTag(title);

    const QString canon = canonicalNorm(norm);
    // normalizeTitle can never emit ':' , so a normalised key is never already namespaced and the prefix is
    // unconditional here (unlike itemId, which has to cope with mergeKey's "pcgame:rawtitle/" fallback —
    // unreachable in this branch, which already returned for an empty norm).
    if (canon != norm) return QStringLiteral("pcgame:") + canon;
    return base;
}

QStringList pcgame::rankMergeCandidates(const QString& title, const QStringList& others)
{
    const QStringList mine = normalizeTitle(title).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    auto sharedPrefix = [&mine](const QString& other) {
        const QStringList t = normalizeTitle(other).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        int n = 0;
        while (n < mine.size() && n < t.size() && mine.at(n) == t.at(n)) ++n;
        return n;
    };
    QVector<QPair<int, QString>> scored;
    scored.reserve(others.size());
    for (const QString& o : others) scored.push_back({ sharedPrefix(o), o });
    // Alphabetical inside a score band, so the order is TOTAL — two entries with the same score would
    // otherwise sit in whatever order the folder happened to build them in, and the picker would reshuffle
    // between two runs over an unchanged library.
    std::stable_sort(scored.begin(), scored.end(),
                     [](const QPair<int, QString>& a, const QPair<int, QString>& b) {
                         if (a.first != b.first) return a.first > b.first;   // more shared words first
                         return QString::compare(a.second, b.second, Qt::CaseInsensitive) < 0;
                     });
    QStringList out;
    out.reserve(scored.size());
    for (const QPair<int, QString>& p : scored) out << p.second;
    return out;
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
