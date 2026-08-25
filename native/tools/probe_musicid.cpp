// Headless check of CROSS-SOURCE MUSIC IDENTITY (issue #194): the thing that decides whether the album on
// this disk and the album on a Navidrome box are one record or two.
//
// The property that matters more than everything else here, and is pinned from BOTH sides:
//
//     A WRONG MERGE HIDES MUSIC. A MISSED MERGE SHOWS A DUPLICATE.
//
// Those costs are not symmetric, so the matcher is biased to refuse — and a matcher that is biased to refuse
// passes every over-merge test by doing nothing at all. So every section below that proves something does
// NOT merge is paired with one that proves the thing it is supposed to merge still does, and the mutation
// list (native/tools/musicid-mutants.json) mutates in both directions for the same reason: a mutant that
// merges too eagerly must die, and so must one that stops merging altogether.
//
// Prints MUSICID-OK on success; any failure prints MUSICID-FAIL <cond> and exits non-zero.
#include "MusicId.h"
#include "MusicMerge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "MUSICID-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using namespace MusicId;

// ---------------------------------------------------------------------------------------------------------
// Fixture helpers. MusicLibrary::Index values are built BY HAND rather than by scanning: MusicLibrary.cpp
// pulls AudioTags and TagLib, and a probe that needed a tag reader to run is a probe nobody runs in a loop.
// Index::artist()/album() live in that same .cpp, so the lookups below are local too.
// ---------------------------------------------------------------------------------------------------------
static MusicLibrary::Album mkAlbum(const QString& key, const QString& artist, const QString& title,
                                   int year, int tracks,
                                   const QString& mbRelease = QString(),
                                   const QString& mbGroup = QString())
{
    MusicLibrary::Album b;
    b.key = key; b.albumArtist = artist; b.title = title; b.year = year; b.trackCount = tracks;
    b.mbidRelease = mbRelease; b.mbidReleaseGroup = mbGroup;
    for (int i = 1; i <= tracks; ++i)
    {
        MusicLibrary::IndexTrack t;
        t.path = key + QStringLiteral("#") + QString::number(i);
        t.sourcePath = t.path;
        t.title = QStringLiteral("Track %1").arg(i);
        t.albumKey = key; t.track = i;
        b.tracks.push_back(t);
    }
    return b;
}

static MusicLibrary::Artist mkArtist(const QString& key, const QString& name,
                                     const QVector<MusicLibrary::Album>& albums,
                                     const QString& mbid = QString())
{
    MusicLibrary::Artist a;
    a.key = key; a.name = name; a.mbid = mbid; a.albums = albums;
    a.albumCount = int(albums.size());
    for (const MusicLibrary::Album& b : albums) a.trackCount += int(b.tracks.size());
    return a;
}

static const MusicLibrary::Artist* artistIn(const MusicLibrary::Index& idx, const QString& key)
{
    for (const MusicLibrary::Artist& a : idx.artists) if (a.key == key) return &a;
    return nullptr;
}
static const MusicLibrary::Album* albumIn(const MusicLibrary::Index& idx, const QString& key)
{
    for (const MusicLibrary::Artist& a : idx.artists)
        for (const MusicLibrary::Album& b : a.albums) if (b.key == key) return &b;
    return nullptr;
}
static int albumCountIn(const MusicLibrary::Index& idx)
{
    int n = 0;
    for (const MusicLibrary::Artist& a : idx.artists) n += int(a.albums.size());
    return n;
}

static AlbumFacts facts(const QString& artist, const QString& title, int year = 0, int tracks = 0,
                        const QString& rel = QString(), const QString& grp = QString(),
                        const QString& artistMbid = QString())
{
    AlbumFacts f;
    f.albumArtist = artist; f.title = title; f.year = year; f.trackCount = tracks;
    f.mbid.release = rel; f.mbid.releaseGroup = grp; f.artistMbid = artistMbid;
    return f;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString ini = QDir::temp().filePath(QStringLiteral("eb-probe-musicid.ini"));
    QFile::remove(ini);
    MusicId::setIniPathForTesting(ini);

    // =====================================================================================================
    // 1. normalizeArtist
    // =====================================================================================================
    CHECK(normalizeArtist(QStringLiteral("Radiohead")) == QStringLiteral("radiohead"));
    // Case and diacritics: the two spellings a rip and a server routinely disagree on.
    CHECK(normalizeArtist(QStringLiteral("BEYONCÉ")) == normalizeArtist(QStringLiteral("Beyonce")));
    CHECK(normalizeArtist(QStringLiteral("Mötley Crüe")) == QStringLiteral("motley crue"));
    CHECK(normalizeArtist(QStringLiteral("Sigur Rós")) == QStringLiteral("sigur ros"));
    // "&" is read as "and", both ways round.
    CHECK(normalizeArtist(QStringLiteral("Simon & Garfunkel"))
          == normalizeArtist(QStringLiteral("Simon and Garfunkel")));
    // Featured-artist noise, bracketed or not.
    CHECK(normalizeArtist(QStringLiteral("Jay-Z feat. Alicia Keys")) == QStringLiteral("jay z"));
    CHECK(normalizeArtist(QStringLiteral("Jay-Z (feat. Alicia Keys)")) == QStringLiteral("jay z"));
    CHECK(normalizeArtist(QStringLiteral("Jay-Z ft Alicia Keys")) == QStringLiteral("jay z"));
    CHECK(normalizeArtist(QStringLiteral("Jay-Z featuring Alicia Keys")) == QStringLiteral("jay z"));
    // ...but never at the front: an act whose name opens with one of those words is a name, not a credit.
    CHECK(!normalizeArtist(QStringLiteral("Feat. Records")).isEmpty());
    // The leading article, dropped — and never to nothing.
    CHECK(normalizeArtist(QStringLiteral("The Beatles")) == normalizeArtist(QStringLiteral("Beatles")));
    CHECK(normalizeArtist(QStringLiteral("The")) == QStringLiteral("the"));
    CHECK(normalizeArtist(QStringLiteral("The The")) == QStringLiteral("the"));
    // NUMERALS ARE NAMES HERE, never sequels: nothing is levelled, nothing is stripped.
    CHECK(normalizeArtist(QStringLiteral("Blink-182")) == QStringLiteral("blink 182"));
    CHECK(normalizeArtist(QStringLiteral("Sum 41")) == QStringLiteral("sum 41"));
    CHECK(normalizeArtist(QStringLiteral("Maroon 5")) != normalizeArtist(QStringLiteral("Maroon V")));
    // The compilation bucket: one artist, spelled the same on both sides.
    CHECK(normalizeArtist(QStringLiteral("Various Artists")) == QStringLiteral("various artists"));
    // An unnameable artist normalises to nothing rather than to a bucket everyone shares.
    CHECK(normalizeArtist(QStringLiteral("   ")).isEmpty());
    CHECK(normalizeArtist(QStringLiteral("!!!")) == QStringLiteral(""));

    // =====================================================================================================
    // 2. normalizeAlbum — the traps
    // =====================================================================================================
    CHECK(normalizeAlbum(QStringLiteral("Kid A")) == QStringLiteral("kid a"));

    // ---- Edition and remaster noise DOES go ----
    CHECK(normalizeAlbum(QStringLiteral("OK Computer (Deluxe Edition)")) == QStringLiteral("ok computer"));
    CHECK(normalizeAlbum(QStringLiteral("OK Computer [2009 Remaster]")) == QStringLiteral("ok computer"));
    CHECK(normalizeAlbum(QStringLiteral("OK Computer (Remastered)")) == QStringLiteral("ok computer"));
    CHECK(normalizeAlbum(QStringLiteral("OK Computer - Remastered 2011")) == QStringLiteral("ok computer"));
    CHECK(normalizeAlbum(QStringLiteral("OK Computer: Deluxe Edition")) == QStringLiteral("ok computer"));
    CHECK(normalizeAlbum(QStringLiteral("OK Computer (Bonus Track Version)")) == QStringLiteral("ok computer"));
    CHECK(normalizeAlbum(QStringLiteral("OK Computer (20th Anniversary Edition)")) == QStringLiteral("ok computer"));
    CHECK(normalizeAlbum(QStringLiteral("OK Computer Deluxe Edition")) == QStringLiteral("ok computer"));
    CHECK(normalizeAlbum(QStringLiteral("OK Computer (Original Recording Remastered)"))
          == QStringLiteral("ok computer"));
    // Stacked noise peels all the way.
    CHECK(normalizeAlbum(QStringLiteral("OK Computer (Deluxe Edition) [Remastered]"))
          == QStringLiteral("ok computer"));

    // ---- ...and everything that is NOT noise STAYS. Each of these can be the whole difference between two
    //      records the user owns separately, and dropping one is the expensive direction.
    CHECK(normalizeAlbum(QStringLiteral("Rattle and Hum (Live)")) != normalizeAlbum(QStringLiteral("Rattle and Hum")));
    CHECK(normalizeAlbum(QStringLiteral("Pet Sounds (Mono)")) != normalizeAlbum(QStringLiteral("Pet Sounds")));
    CHECK(normalizeAlbum(QStringLiteral("Nebraska (Demo)")) != normalizeAlbum(QStringLiteral("Nebraska")));
    CHECK(normalizeAlbum(QStringLiteral("Unplugged (Acoustic)")) != normalizeAlbum(QStringLiteral("Unplugged")));
    CHECK(normalizeAlbum(QStringLiteral("Blue (1971)")) != normalizeAlbum(QStringLiteral("Blue")));
    CHECK(normalizeAlbum(QStringLiteral("Homework (Remixes)")) != normalizeAlbum(QStringLiteral("Homework")));
    // A trailing segment that is not noise is left exactly where it is.
    CHECK(normalizeAlbum(QStringLiteral("Under the Covers - Live at Wembley"))
          != normalizeAlbum(QStringLiteral("Under the Covers")));
    // An album genuinely CALLED a noise word does not normalise to nothing.
    CHECK(normalizeAlbum(QStringLiteral("Bonus")) == QStringLiteral("bonus"));
    CHECK(normalizeAlbum(QStringLiteral("Deluxe")) == QStringLiteral("deluxe"));
    CHECK(normalizeAlbum(QStringLiteral("Christmas Special")) == QStringLiteral("christmas special"));
    CHECK(normalizeAlbum(QStringLiteral("The Original Recordings"))
          == QStringLiteral("the original recordings"));

    // ---- Vol. / Volume / Vol, and Pt. / Part ----
    CHECK(normalizeAlbum(QStringLiteral("Greatest Hits Vol. 2"))
          == normalizeAlbum(QStringLiteral("Greatest Hits Volume 2")));
    CHECK(normalizeAlbum(QStringLiteral("Greatest Hits Vol 2"))
          == normalizeAlbum(QStringLiteral("Greatest Hits Volume 2")));
    CHECK(normalizeAlbum(QStringLiteral("Chapter Pt. II")) == normalizeAlbum(QStringLiteral("Chapter Part 2")));
    CHECK(normalizeAlbum(QStringLiteral("Sessions Vol. V")) == normalizeAlbum(QStringLiteral("Sessions Volume 5")));

    // ---- THE SEQUEL GUARD. A number is never stripped and volume 2 never becomes volume 1. ----
    CHECK(normalizeAlbum(QStringLiteral("Greatest Hits Volume 2"))
          != normalizeAlbum(QStringLiteral("Greatest Hits Volume 1")));
    CHECK(normalizeAlbum(QStringLiteral("Greatest Hits Vol. II"))
          != normalizeAlbum(QStringLiteral("Greatest Hits Vol. I")));
    CHECK(normalizeAlbum(QStringLiteral("Chapter 2")) != normalizeAlbum(QStringLiteral("Chapter")));
    CHECK(normalizeAlbum(QStringLiteral("Led Zeppelin IV")) != normalizeAlbum(QStringLiteral("Led Zeppelin III")));
    // Roman levelling is applied to BOTH sides, which is the half the romhack matcher in this repo got wrong.
    CHECK(normalizeAlbum(QStringLiteral("Led Zeppelin IV")) == normalizeAlbum(QStringLiteral("Led Zeppelin 4")));

    // ---- The Roman-numeral value cap, which is what keeps ordinary words out of the levelling ----
    CHECK(romanValue(QStringLiteral("iv")) == 4);
    CHECK(romanValue(QStringLiteral("xxx")) == 30);
    CHECK(romanValue(QStringLiteral("mix")) == 1009);        // a real word AND a valid numeral
    CHECK(romanValue(QStringLiteral("cd")) == 400);
    CHECK(romanValue(QStringLiteral("iiii")) == 0);          // strict subtractive form only
    CHECK(romanValue(QStringLiteral("hits")) == 0);
    CHECK(normalizeAlbum(QStringLiteral("Discovery Mix")) == QStringLiteral("discovery mix"));
    CHECK(normalizeAlbum(QStringLiteral("Bootleg CD")) == QStringLiteral("bootleg cd"));
    // A single-token title is never levelled: the album called "I" must stay away from the album called "1".
    CHECK(normalizeAlbum(QStringLiteral("I")) != normalizeAlbum(QStringLiteral("1")));
    CHECK(normalizeAlbum(QStringLiteral("X")) != normalizeAlbum(QStringLiteral("10")));
    // ...including a two-character one, which is the case the "never at index 0" rule is actually carrying:
    // the album called "II" and the album called "2" are two records until something says otherwise.
    CHECK(normalizeAlbum(QStringLiteral("II")) != normalizeAlbum(QStringLiteral("2")));

    // ---- Case, diacritics, "&", trademark marks ----
    CHECK(normalizeAlbum(QStringLiteral("BORN IN THE U.S.A."))
          == normalizeAlbum(QStringLiteral("Born in the USA")));
    // A run of single letters is ONE word; a lone one is an ordinary word and must not be glued to anything.
    CHECK(normalizeArtist(QStringLiteral("R.E.M.")) == normalizeArtist(QStringLiteral("REM")));
    CHECK(normalizeAlbum(QStringLiteral("L.A. Woman")) == normalizeAlbum(QStringLiteral("LA Woman")));
    CHECK(normalizeAlbum(QStringLiteral("Kid A")) == QStringLiteral("kid a"));
    CHECK(normalizeArtist(QStringLiteral("Guns N' Roses")) == normalizeArtist(QStringLiteral("Guns N Roses")));
    // The apostrophe is removed rather than split on, so the two ways a tagger writes a possessive agree.
    CHECK(normalizeAlbum((QStringLiteral("Sgt. Pepper") + QChar(0x2019) + QStringLiteral("s Lonely Hearts Club Band")))
          == normalizeAlbum(QStringLiteral("Sgt Peppers Lonely Hearts Club Band")));
    CHECK(normalizeAlbum(QStringLiteral("Café Bleu")) == normalizeAlbum(QStringLiteral("Cafe Bleu")));
    CHECK(normalizeAlbum(QStringLiteral("Rum, Sodomy & the Lash"))
          == normalizeAlbum(QStringLiteral("Rum, Sodomy and the Lash")));
    CHECK(normalizeAlbum(QStringLiteral("Thriller™")) == QStringLiteral("thriller"));
    CHECK(normalizeAlbum(QString::fromUtf8("Don\xe2\x80\x99t Look Back"))
          == normalizeAlbum(QStringLiteral("Don't Look Back")));
    // NO leading-article strip for an album — deliberately asymmetric with the artist rule.
    CHECK(normalizeAlbum(QStringLiteral("The Wall")) != normalizeAlbum(QStringLiteral("Wall")));

    // =====================================================================================================
    // 3. MusicBrainz ground truth
    // =====================================================================================================
    CHECK(groundArtist(QStringLiteral("A"), QStringLiteral("a")) == Ground::Same);   // case-insensitive
    CHECK(groundArtist(QStringLiteral("A"), QStringLiteral("B")) == Ground::Different);
    CHECK(groundArtist(QStringLiteral("A"), QString()) == Ground::Silent);
    CHECK(groundArtist(QString(), QString()) == Ground::Silent);
    {
        AlbumMbid a, b;
        a.releaseGroup = QStringLiteral("G1"); a.release = QStringLiteral("R1");
        b.releaseGroup = QStringLiteral("G1"); b.release = QStringLiteral("R2");
        // The GROUP wins when both sides have one: two releases of one album are one album.
        CHECK(groundAlbum(a, b) == Ground::Same);
        b.releaseGroup = QStringLiteral("G2");
        CHECK(groundAlbum(a, b) == Ground::Different);
        // Only one side has a group -> fall to the releases, never compare a group against a release.
        b.releaseGroup.clear(); b.release = QStringLiteral("R1");
        CHECK(groundAlbum(a, b) == Ground::Same);
        b.release = QStringLiteral("R9");
        CHECK(groundAlbum(a, b) == Ground::Different);
        // Nothing comparable at all.
        AlbumMbid empty;
        CHECK(groundAlbum(a, empty) == Ground::Silent);
        CHECK(groundAlbum(empty, empty) == Ground::Silent);
    }

    // =====================================================================================================
    // 4. The confidence rules
    // =====================================================================================================
    // The strings agree -> Likely, which merges.
    CHECK(merges(artistConfidence(QStringLiteral("The Beatles"), QString(),
                                  QStringLiteral("Beatles"), QString())));
    // An MBID that DISAGREES beats the strings, and refuses.
    CHECK(!merges(artistConfidence(QStringLiteral("Nirvana"), QStringLiteral("mb-a"),
                                   QStringLiteral("Nirvana"), QStringLiteral("mb-b"))));
    // ...and one that AGREES beats them the other way, so two spellings still merge.
    CHECK(merges(artistConfidence(QStringLiteral("Prince"), QStringLiteral("mb-x"),
                                  QString::fromUtf8("\xe2\x99\xac"), QStringLiteral("mb-x"))));
    // An unnameable artist matches nothing, INCLUDING another unnameable one.
    CHECK(!merges(artistConfidence(QString(), QString(), QString(), QString())));

    // Albums: same artist, same title, compatible year.
    CHECK(merges(albumConfidence(facts(QStringLiteral("Radiohead"), QStringLiteral("OK Computer"), 1997, 12),
                                 facts(QStringLiteral("Radiohead"),
                                       QStringLiteral("OK Computer (Deluxe Edition)"), 1997, 23))));
    // A DIFFERENT TRACK COUNT IS NOT A GATE: the bonus disc is the same album and the user wants it merged.
    CHECK(merges(albumConfidence(facts(QStringLiteral("Radiohead"), QStringLiteral("OK Computer"), 1997, 12),
                                 facts(QStringLiteral("Radiohead"), QStringLiteral("OK Computer"), 1997, 23))));
    // One year of slack absorbs ordinary release-date drift.
    CHECK(merges(albumConfidence(facts(QStringLiteral("Radiohead"), QStringLiteral("OK Computer"), 1997),
                                 facts(QStringLiteral("Radiohead"), QStringLiteral("OK Computer"), 1998))));
    // THE YEAR GATE. This is what separates a live record from the studio album it is named after when
    // nothing in the title says so, and a 1973 album from a 2009 remaster LISTING.
    CHECK(!merges(albumConfidence(facts(QStringLiteral("U2"), QStringLiteral("Under a Blood Red Sky"), 1983),
                                  facts(QStringLiteral("U2"), QStringLiteral("Under a Blood Red Sky"), 1980))));
    // An unknown year is compatible with everything: absence is not disagreement.
    CHECK(merges(albumConfidence(facts(QStringLiteral("U2"), QStringLiteral("Boy"), 1980),
                                 facts(QStringLiteral("U2"), QStringLiteral("Boy"), 0))));
    // A different artist refuses even with an identical title — self-titled records are why.
    CHECK(!merges(albumConfidence(facts(QStringLiteral("Weezer"), QStringLiteral("Weezer"), 1994),
                                  facts(QStringLiteral("Nirvana"), QStringLiteral("Weezer"), 1994))));
    // A different title refuses even with an identical artist and year.
    CHECK(!merges(albumConfidence(facts(QStringLiteral("Radiohead"), QStringLiteral("Kid A"), 2000),
                                  facts(QStringLiteral("Radiohead"), QStringLiteral("Amnesiac"), 2000))));
    // An album MBID that disagrees refuses even when everything else lines up perfectly.
    CHECK(!merges(albumConfidence(facts(QStringLiteral("Radiohead"), QStringLiteral("Kid A"), 2000, 10,
                                        QStringLiteral("rel-1")),
                                  facts(QStringLiteral("Radiohead"), QStringLiteral("Kid A"), 2000, 10,
                                        QStringLiteral("rel-2")))));
    // ...and one that AGREES merges even when the year gate would have refused: ground truth beats a heuristic.
    CHECK(merges(albumConfidence(facts(QStringLiteral("Radiohead"), QStringLiteral("Kid A"), 2000, 10,
                                       QStringLiteral("rel-1")),
                                 facts(QStringLiteral("Radiohead"), QStringLiteral("Kid A [2019 Reissue]"),
                                       2019, 10, QStringLiteral("rel-1")))));
    // A compilation: one artist, one album, on both sides.
    CHECK(merges(albumConfidence(facts(QStringLiteral("Various Artists"), QStringLiteral("Now 40"), 1998, 40),
                                 facts(QStringLiteral("various artists"), QStringLiteral("NOW 40"), 1998, 40))));
    // An untitled album matches nothing rather than every other untitled album.
    CHECK(!merges(albumConfidence(facts(QStringLiteral("Radiohead"), QString(), 2000),
                                  facts(QStringLiteral("Radiohead"), QString(), 2000))));
    // ...and neither does one with no artist.
    CHECK(!merges(albumConfidence(facts(QString(), QStringLiteral("Kid A"), 2000),
                                  facts(QString(), QStringLiteral("Kid A"), 2000))));
    // Volume 2 never becomes volume 1, at the predicate level too.
    CHECK(!merges(albumConfidence(facts(QStringLiteral("Ministry of Sound"), QStringLiteral("Vol. 2"), 2001),
                                  facts(QStringLiteral("Ministry of Sound"), QStringLiteral("Volume 1"), 2001))));
    CHECK(merges(albumConfidence(facts(QStringLiteral("Ministry of Sound"), QStringLiteral("Vol. 2"), 2001),
                                 facts(QStringLiteral("Ministry of Sound"), QStringLiteral("Volume 2"), 2001))));

    // =====================================================================================================
    // 5. closeness — the tiebreak, and ONLY a tiebreak
    // =====================================================================================================
    {
        const AlbumFacts base = facts(QStringLiteral("A"), QStringLiteral("B"), 2000, 12);
        AlbumFacts near = facts(QStringLiteral("A"), QStringLiteral("B"), 2000, 12);
        AlbumFacts far  = facts(QStringLiteral("A"), QStringLiteral("B"), 2000, 23);
        CHECK(closeness(base, near) < closeness(base, far));
        near.durationSec = 2400; far.durationSec = 2410;
        AlbumFacts b2 = base; b2.durationSec = 2400;
        far.trackCount = 12;
        CHECK(closeness(b2, near) < closeness(b2, far));   // duration breaks a track-count tie
        // An unknown count contributes nothing rather than a penalty.
        AlbumFacts unknown = facts(QStringLiteral("A"), QStringLiteral("B"), 2000, 0);
        CHECK(closeness(base, unknown) == 0);
    }

    // =====================================================================================================
    // 6. The manual override store
    // =====================================================================================================
    CHECK(albumOverrideVerdict(QStringLiteral("A"), QStringLiteral("X"),
                               QStringLiteral("B"), QStringLiteral("Y")) == -1);
    setAlbumOverride(QStringLiteral("A"), QStringLiteral("X"), QStringLiteral("B"), QStringLiteral("Y"), true);
    CHECK(albumOverrideVerdict(QStringLiteral("A"), QStringLiteral("X"),
                               QStringLiteral("B"), QStringLiteral("Y")) == 1);
    // SYMMETRIC by construction, not by a second lookup somebody can forget.
    CHECK(albumOverrideVerdict(QStringLiteral("B"), QStringLiteral("Y"),
                               QStringLiteral("A"), QStringLiteral("X")) == 1);
    // ...and it beats the matcher, in the direction that says "merge".
    CHECK(merges(albumConfidence(facts(QStringLiteral("A"), QStringLiteral("X")),
                                 facts(QStringLiteral("B"), QStringLiteral("Y")))));
    // A NEGATIVE verdict, and the self-pair form it takes when the two copies key the same — which is the
    // ordinary case, because that is exactly why they merged.
    setAlbumOverride(QStringLiteral("Radiohead"), QStringLiteral("Kid A"),
                     QStringLiteral("Radiohead"), QStringLiteral("Kid A (Remastered)"), false);
    CHECK(!merges(albumConfidence(facts(QStringLiteral("Radiohead"), QStringLiteral("Kid A"), 2000),
                                  facts(QStringLiteral("Radiohead"), QStringLiteral("Kid A (Remastered)"),
                                        2000))));
    // CLEARING IS NOT THE SAME AS SAYING "NOT THE SAME": it hands the decision back to the matcher.
    clearAlbumOverride(QStringLiteral("Radiohead"), QStringLiteral("Kid A"),
                       QStringLiteral("Radiohead"), QStringLiteral("Kid A (Remastered)"));
    CHECK(albumOverrideVerdict(QStringLiteral("Radiohead"), QStringLiteral("Kid A"),
                               QStringLiteral("Radiohead"), QStringLiteral("Kid A (Remastered)")) == -1);
    CHECK(merges(albumConfidence(facts(QStringLiteral("Radiohead"), QStringLiteral("Kid A"), 2000),
                                 facts(QStringLiteral("Radiohead"), QStringLiteral("Kid A (Remastered)"), 2000))));
    // The stored key is normalizeArtist!normalizeAlbum, applied EXACTLY ONCE — a raw title and its own
    // normalised form must therefore reach the same verdict, or every read would silently miss.
    CHECK(albumKeyOf(QStringLiteral("The Beatles"), QStringLiteral("Revolver (Remastered)"))
          == QStringLiteral("beatles!revolver"));
    CHECK(albumKeyOf(QString(), QStringLiteral("Revolver")).isEmpty());
    // overrides() lists what is stored, in the stored form, and clearing BY KEY removes it.
    {
        const QVector<Verdict> all = albumOverrides();
        bool found = false;
        for (const Verdict& v : all)
            if ((v.a == QStringLiteral("a!x") && v.b == QStringLiteral("b!y"))
                || (v.a == QStringLiteral("b!y") && v.b == QStringLiteral("a!x"))) found = true;
        CHECK(found);
        clearAlbumOverrideKeys(QStringLiteral("a!x"), QStringLiteral("b!y"));
        CHECK(albumOverrideVerdict(QStringLiteral("A"), QStringLiteral("X"),
                                   QStringLiteral("B"), QStringLiteral("Y")) == -1);
    }
    // Artists take verdicts by the same rules.
    CHECK(merges(artistConfidence(QStringLiteral("Genesis"), QString(),
                                  QStringLiteral("Genesis"), QString())));   // merge by default
    setArtistOverride(QStringLiteral("Genesis"), QStringLiteral("Genesis"), false);
    CHECK(!merges(artistConfidence(QStringLiteral("Genesis"), QString(),
                                   QStringLiteral("Genesis"), QString())));
    clearArtistOverride(QStringLiteral("Genesis"), QStringLiteral("Genesis"));
    CHECK(merges(artistConfidence(QStringLiteral("Genesis"), QString(),
                                  QStringLiteral("Genesis"), QString())));

    // =====================================================================================================
    // 7. pickAutoSource
    // =====================================================================================================
    {
        QVector<SourceRef> all;
        all.push_back({ QString(), true });                       // 0: local
        all.push_back({ QStringLiteral("srv-a"), true });          // 1
        all.push_back({ QStringLiteral("srv-b"), true });          // 2
        CHECK(pickAutoSource(all, QStringLiteral("local")) == 0);
        CHECK(pickAutoSource(all, QStringLiteral("server")) == 1); // any server: the order they were added
        CHECK(pickAutoSource(all, QStringLiteral("srv-b")) == 2);
        // An unrecognised preference reads as local rather than as an error.
        CHECK(pickAutoSource(all, QStringLiteral("srv-gone")) == 0);
        CHECK(pickAutoSource(all, QString()) == 0);
        // AVAILABILITY BEATS THE PREFERENCE: a source that cannot answer must never be chosen to play.
        all[0].available = false;
        CHECK(pickAutoSource(all, QStringLiteral("local")) != 0);
        // Deterministic: the same input, the same answer, every time.
        CHECK(pickAutoSource(all, QStringLiteral("server")) == pickAutoSource(all, QStringLiteral("server")));
        CHECK(pickAutoSource(QVector<SourceRef>(), QStringLiteral("local")) == -1);
        // Local-only, remote-only.
        QVector<SourceRef> onlyLocal;  onlyLocal.push_back({ QString(), true });
        QVector<SourceRef> onlyRemote; onlyRemote.push_back({ QStringLiteral("srv-a"), true });
        CHECK(pickAutoSource(onlyLocal, QStringLiteral("server")) == 0);
        CHECK(pickAutoSource(onlyRemote, QStringLiteral("local")) == 0);
    }

    // =====================================================================================================
    // 8. merge() — the single-source guarantee
    // =====================================================================================================
    // THE CLAIM "a user with only a local library sees exactly what they see today", made checkable. The one
    // supplier's index comes back out with the same artists, the same albums, the same keys, the same order
    // and the same counts, and `active` is false so no caller can take a merged branch over it.
    {
        MusicLibrary::Index local;
        local.artists << mkArtist(QStringLiteral("radiohead"), QStringLiteral("Radiohead"),
                                  { mkAlbum(QStringLiteral("radiohead\x1ft\x1fok computer"),
                                            QStringLiteral("Radiohead"), QStringLiteral("OK Computer"), 1997, 12),
                                    mkAlbum(QStringLiteral("radiohead\x1ft\x1fkid a"),
                                            QStringLiteral("Radiohead"), QStringLiteral("Kid A"), 2000, 10) })
                       << mkArtist(QStringLiteral("aphex twin"), QStringLiteral("Aphex Twin"),
                                   { mkAlbum(QStringLiteral("aphex\x1ft\x1fsaw"), QStringLiteral("Aphex Twin"),
                                             QStringLiteral("Selected Ambient Works"), 1992, 13) });
        local.trackCount = 35;

        MusicLibrary::Index emptyServer;
        QVector<MusicMerge::Source> srcs;
        srcs.push_back({ QString(), &local });
        srcs.push_back({ QStringLiteral("srv-a"), &emptyServer });
        const MusicMerge::Merged m = MusicMerge::merge(srcs, QStringLiteral("local"));
        CHECK(!m.active);
        CHECK(m.artistGroup.isEmpty() && m.albumGroup.isEmpty());
        CHECK(m.idx.artists.size() == local.artists.size());
        for (int i = 0; i < m.idx.artists.size(); ++i)
        {
            CHECK(m.idx.artists.at(i).key == local.artists.at(i).key);
            CHECK(m.idx.artists.at(i).name == local.artists.at(i).name);
            CHECK(m.idx.artists.at(i).albumCount == local.artists.at(i).albumCount);
            CHECK(m.idx.artists.at(i).trackCount == local.artists.at(i).trackCount);
            CHECK(m.idx.artists.at(i).albums.size() == local.artists.at(i).albums.size());
            for (int j = 0; j < m.idx.artists.at(i).albums.size(); ++j)
                CHECK(m.idx.artists.at(i).albums.at(j).key == local.artists.at(i).albums.at(j).key);
        }
        CHECK(m.idx.trackCount == local.trackCount);
        // instancesOf() answers for a key that never merged, so no caller needs a special case.
        CHECK(m.albumInstances(QStringLiteral("radiohead\x1ft\x1fkid a"))
              == (QStringList{ QStringLiteral("radiohead\x1ft\x1fkid a") }));
        // A SERVER-ONLY install is the same claim from the other side.
        QVector<MusicMerge::Source> only;
        MusicLibrary::Index emptyLocal;
        only.push_back({ QString(), &emptyLocal });
        only.push_back({ QStringLiteral("srv-a"), &local });
        const MusicMerge::Merged m2 = MusicMerge::merge(only, QStringLiteral("local"));
        CHECK(!m2.active);
        CHECK(m2.idx.artists.size() == local.artists.size());
    }

    // =====================================================================================================
    // 9. merge() — two suppliers
    // =====================================================================================================
    {
        MusicLibrary::Index local;
        local.artists
            << mkArtist(QStringLiteral("radiohead"), QStringLiteral("Radiohead"),
                        { mkAlbum(QStringLiteral("L:okc"), QStringLiteral("Radiohead"),
                                  QStringLiteral("OK Computer"), 1997, 12),
                          mkAlbum(QStringLiteral("L:kida"), QStringLiteral("Radiohead"),
                                  QStringLiteral("Kid A"), 2000, 10) })
            << mkArtist(QStringLiteral("boards"), QStringLiteral("Boards of Canada"),
                        { mkAlbum(QStringLiteral("L:mhtrtc"), QStringLiteral("Boards of Canada"),
                                  QStringLiteral("Music Has the Right to Children"), 1998, 17) });
        local.trackCount = 39;

        MusicLibrary::Index srv;
        srv.artists
            << mkArtist(QStringLiteral("S:rh"), QStringLiteral("radiohead"),
                        { // the SAME record, spelled the way a server tagged it
                          mkAlbum(QStringLiteral("S:okc"), QStringLiteral("Radiohead"),
                                  QStringLiteral("OK Computer [2009 Remaster]"), 1997, 12),
                          // ...and one the local library does not have at all
                          mkAlbum(QStringLiteral("S:amn"), QStringLiteral("Radiohead"),
                                  QStringLiteral("Amnesiac"), 2001, 11),
                          // ...and a LIVE record whose title matches nothing here, which must stay apart
                          mkAlbum(QStringLiteral("S:kidalive"), QStringLiteral("Radiohead"),
                                  QStringLiteral("Kid A (Live)"), 2000, 10) })
            << mkArtist(QStringLiteral("S:aut"), QStringLiteral("Autechre"),
                        { mkAlbum(QStringLiteral("S:tri"), QStringLiteral("Autechre"),
                                  QStringLiteral("Tri Repetae"), 1995, 11) });

        QVector<MusicMerge::Source> srcs;
        srcs.push_back({ QString(), &local });
        srcs.push_back({ QStringLiteral("srv-a"), &srv });
        const MusicMerge::Merged m = MusicMerge::merge(srcs, QStringLiteral("local"));

        CHECK(m.active);
        // ONE Radiohead, not two. Three artists in total: Radiohead, Autechre, Boards of Canada.
        CHECK(m.idx.artists.size() == 3);
        CHECK(artistIn(m.idx, QStringLiteral("radiohead")) != nullptr);   // local won the pick
        CHECK(artistIn(m.idx, QStringLiteral("S:rh")) == nullptr);
        CHECK(m.artistInstances(QStringLiteral("radiohead")).size() == 2);
        CHECK(m.artistInstances(QStringLiteral("radiohead")).first() == QStringLiteral("radiohead"));
        CHECK(m.sourceOf.value(QStringLiteral("S:rh")) == QStringLiteral("srv-a"));
        CHECK(m.sourceOf.value(QStringLiteral("radiohead")).isEmpty());

        // ONE OK Computer, keyed on the local copy; the server copy is reachable as an instance.
        const MusicLibrary::Artist* rh = artistIn(m.idx, QStringLiteral("radiohead"));
        CHECK(rh != nullptr);
        if (rh)
        {
            // OK Computer (merged), Kid A, Kid A (Live), Amnesiac = four rows.
            CHECK(rh->albums.size() == 4);
            CHECK(albumIn(m.idx, QStringLiteral("L:okc")) != nullptr);
            CHECK(albumIn(m.idx, QStringLiteral("S:okc")) == nullptr);
            CHECK(m.albumInstances(QStringLiteral("L:okc"))
                  == (QStringList{ QStringLiteral("L:okc"), QStringLiteral("S:okc") }));
            // The album only the server has is present, under its own key.
            CHECK(albumIn(m.idx, QStringLiteral("S:amn")) != nullptr);
            // THE PAIR THAT MUST NOT MERGE. "Kid A" and "Kid A (Live)" share an artist and a year and
            // differ only by a word this normaliser is required NOT to treat as noise.
            CHECK(albumIn(m.idx, QStringLiteral("L:kida")) != nullptr);
            CHECK(albumIn(m.idx, QStringLiteral("S:kidalive")) != nullptr);
            CHECK(m.albumInstances(QStringLiteral("L:kida")).size() == 1);
            // Albums are ordered oldest first, as everywhere else.
            CHECK(rh->albums.first().year == 1997);
            CHECK(rh->albums.last().year == 2001);
        }
        // Artists are ordered alphabetically, as everywhere else.
        CHECK(m.idx.artists.first().name == QStringLiteral("Autechre"));
        // Shuffle-all still counts the LOCAL library, which is the only thing it can queue.
        CHECK(m.idx.trackCount == 39);

        // THE PREFERENCE DECIDES WHICH COPY IS KEYED AND PLAYED, and nothing else changes.
        const MusicMerge::Merged ms = MusicMerge::merge(srcs, QStringLiteral("server"));
        CHECK(ms.idx.artists.size() == 3);
        CHECK(artistIn(ms.idx, QStringLiteral("S:rh")) != nullptr);
        CHECK(artistIn(ms.idx, QStringLiteral("radiohead")) == nullptr);
        CHECK(albumIn(ms.idx, QStringLiteral("S:okc")) != nullptr);
        CHECK(ms.albumInstances(QStringLiteral("S:okc")).first() == QStringLiteral("S:okc"));
        // Determinism: the same inputs give the same answer, so a merged row's identity does not flap.
        const MusicMerge::Merged again = MusicMerge::merge(srcs, QStringLiteral("local"));
        CHECK(again.idx.artists.size() == m.idx.artists.size());
        CHECK(again.albumInstances(QStringLiteral("L:okc")) == m.albumInstances(QStringLiteral("L:okc")));
    }

    // =====================================================================================================
    // 10. merge() — two instances from the SAME supplier never merge, even transitively
    // =====================================================================================================
    {
        // A supplier that lists a record twice under two spellings has said the user owns two records.
        MusicLibrary::Index local;
        local.artists << mkArtist(QStringLiteral("a"), QStringLiteral("Portishead"),
                                  { mkAlbum(QStringLiteral("L:dummy1"), QStringLiteral("Portishead"),
                                            QStringLiteral("Dummy"), 1994, 11),
                                    mkAlbum(QStringLiteral("L:dummy2"), QStringLiteral("Portishead"),
                                            QStringLiteral("Dummy (Remastered)"), 1994, 11) });
        MusicLibrary::Index srv;
        srv.artists << mkArtist(QStringLiteral("S:a"), QStringLiteral("Portishead"),
                                { mkAlbum(QStringLiteral("S:dummy"), QStringLiteral("Portishead"),
                                          QStringLiteral("Dummy"), 1994, 11) });
        QVector<MusicMerge::Source> srcs;
        srcs.push_back({ QString(), &local });
        srcs.push_back({ QStringLiteral("srv-a"), &srv });
        const MusicMerge::Merged m = MusicMerge::merge(srcs, QStringLiteral("local"));
        // Two rows survive: the server copy joins ONE of the local pair and the other stays its own row.
        CHECK(albumCountIn(m.idx) == 2);
        const int joined = m.albumInstances(QStringLiteral("L:dummy1")).size()
                         + m.albumInstances(QStringLiteral("L:dummy2")).size();
        CHECK(joined == 3);   // one group of 2 + one group of 1
        // ...and no group ever holds two keys from one source.
        for (auto it = m.albumGroup.constBegin(); it != m.albumGroup.constEnd(); ++it)
        {
            QSet<QString> seen;
            for (const QString& k : *it)
            {
                const QString src = m.sourceOf.value(k);
                CHECK(!seen.contains(src));
                seen.insert(src);
            }
        }
    }

    // =====================================================================================================
    // 11. merge() vs a BRUTE-FORCE application of the same predicate
    // =====================================================================================================
    // merge() takes its candidate pairs out of hash buckets rather than comparing everything with everything,
    // and that is complete only because MusicId's predicate can say "merge" ONLY when the MBIDs agree, the
    // normalised keys are equal, or the user said so. That property lives in another file. This is the check
    // that notices when it stops being true.
    {
        MusicLibrary::Index a, b, c;
        a.artists << mkArtist(QStringLiteral("A:1"), QStringLiteral("The Cure"),
                              { mkAlbum(QStringLiteral("A:dis"), QStringLiteral("The Cure"),
                                        QStringLiteral("Disintegration"), 1989, 12),
                                mkAlbum(QStringLiteral("A:fai"), QStringLiteral("The Cure"),
                                        QStringLiteral("Faith"), 1981, 7, QStringLiteral("rel-faith")) })
                  << mkArtist(QStringLiteral("A:2"), QStringLiteral("Various Artists"),
                              { mkAlbum(QStringLiteral("A:now"), QStringLiteral("Various Artists"),
                                        QStringLiteral("Now 40"), 1998, 40) });
        b.artists << mkArtist(QStringLiteral("B:1"), QStringLiteral("Cure"),
                              { mkAlbum(QStringLiteral("B:dis"), QStringLiteral("Cure"),
                                        QStringLiteral("Disintegration (Deluxe Edition)"), 1989, 20),
                                mkAlbum(QStringLiteral("B:fai"), QStringLiteral("Cure"),
                                        QStringLiteral("Faith"), 2005, 7, QStringLiteral("rel-faith")),
                                mkAlbum(QStringLiteral("B:pornography"), QStringLiteral("Cure"),
                                        QStringLiteral("Pornography"), 1982, 8) })
                  << mkArtist(QStringLiteral("B:2"), QStringLiteral("VARIOUS ARTISTS"),
                              { mkAlbum(QStringLiteral("B:now"), QStringLiteral("VARIOUS ARTISTS"),
                                        QStringLiteral("Now 41"), 1999, 40) });
        c.artists << mkArtist(QStringLiteral("C:1"), QString::fromUtf8("The Cur\xc3\xa9"),
                              { mkAlbum(QStringLiteral("C:dis"), QString::fromUtf8("The Cur\xc3\xa9"),
                                        QStringLiteral("Disintegration"), 1989, 12) })
                  << mkArtist(QStringLiteral("C:2"), QStringLiteral("The Cure"),
                              { mkAlbum(QStringLiteral("C:dis2"), QStringLiteral("The Cure"),
                                        QStringLiteral("Disintegration - Remastered"), 1990, 12) });

        QVector<MusicMerge::Source> srcs;
        srcs.push_back({ QString(), &a });
        srcs.push_back({ QStringLiteral("srv-b"), &b });
        srcs.push_back({ QStringLiteral("srv-c"), &c });
        const MusicMerge::Merged m = MusicMerge::merge(srcs, QStringLiteral("local"));

        // Brute force: every album instance against every other, by the same predicate, honouring the same
        // "never two from one source" rule and the same union-order (smaller index wins the root).
        struct Inst { QString src; const MusicLibrary::Album* b; };
        QVector<Inst> all;
        const MusicLibrary::Index* idxs[3] = { &a, &b, &c };
        const QString              ids[3]  = { QString(), QStringLiteral("srv-b"), QStringLiteral("srv-c") };
        for (int s = 0; s < 3; ++s)
            for (const MusicLibrary::Artist& ar : idxs[s]->artists)
                for (const MusicLibrary::Album& al : ar.albums) all.push_back({ ids[s], &al });

        QVector<int>           parent(all.size());
        QVector<QSet<QString>> srcsOf(all.size());
        for (int i = 0; i < all.size(); ++i) { parent[i] = i; srcsOf[i].insert(all.at(i).src); }
        const auto find = [&parent](int x) { while (parent[x] != x) x = parent[x]; return x; };
        for (int i = 0; i < all.size(); ++i)
            for (int j = i + 1; j < all.size(); ++j)
            {
                if (all.at(i).src == all.at(j).src) continue;
                AlbumFacts fi = facts(all.at(i).b->albumArtist, all.at(i).b->title, all.at(i).b->year,
                                      all.at(i).b->trackCount, all.at(i).b->mbidRelease,
                                      all.at(i).b->mbidReleaseGroup);
                AlbumFacts fj = facts(all.at(j).b->albumArtist, all.at(j).b->title, all.at(j).b->year,
                                      all.at(j).b->trackCount, all.at(j).b->mbidRelease,
                                      all.at(j).b->mbidReleaseGroup);
                if (!sameAlbum(fi, fj)) continue;
                const int ri = find(i), rj = find(j);
                if (ri == rj) continue;
                bool clash = false;
                for (const QString& s : srcsOf[rj]) if (srcsOf[ri].contains(s)) clash = true;
                if (clash) continue;
                const int lo = qMin(ri, rj), hi = qMax(ri, rj);
                parent[hi] = lo;
                srcsOf[lo].unite(srcsOf[hi]);
                srcsOf[hi].clear();
            }
        // Same number of album ROWS as the brute force found components.
        QSet<int> roots;
        for (int i = 0; i < all.size(); ++i) roots.insert(find(i));
        CHECK(albumCountIn(m.idx) == int(roots.size()));
        // ...and the same partition: every key merge() grouped together is in one brute-force component.
        QHash<QString, int> rootOfKey;
        for (int i = 0; i < all.size(); ++i) rootOfKey.insert(all.at(i).b->key, find(i));
        for (auto it = m.albumGroup.constBegin(); it != m.albumGroup.constEnd(); ++it)
        {
            const QStringList keys = *it;
            for (const QString& k : keys) CHECK(rootOfKey.value(k, -1) == rootOfKey.value(keys.first(), -2));
        }
        // Faith merges on its MBID DESPITE a 24-year gap that the year gate alone would have refused —
        // the ground truth is doing real work here, not decorating a case the strings already covered.
        CHECK(m.albumInstances(QStringLiteral("A:fai")).size() == 2);
        // "Now 40" and "Now 41" are one digit apart and must never be one row.
        CHECK(m.albumInstances(QStringLiteral("A:now")).size() == 1);
    }

    // =====================================================================================================
    // 12. The override changes what merge() decides — in BOTH directions
    // =====================================================================================================
    {
        MusicLibrary::Index local, srv;
        local.artists << mkArtist(QStringLiteral("L:pj"), QStringLiteral("Pearl Jam"),
                                  { mkAlbum(QStringLiteral("L:ten"), QStringLiteral("Pearl Jam"),
                                            QStringLiteral("Ten"), 1991, 11),
                                    mkAlbum(QStringLiteral("L:vs"), QStringLiteral("Pearl Jam"),
                                            QStringLiteral("Vs."), 1993, 12) });
        srv.artists << mkArtist(QStringLiteral("S:pj"), QStringLiteral("Pearl Jam"),
                                { mkAlbum(QStringLiteral("S:ten"), QStringLiteral("Pearl Jam"),
                                          QStringLiteral("Ten"), 1991, 11),
                                  // A copy the matcher REFUSES: the same record spelled another way,
                                  // which no string rule can safely equate.
                                  mkAlbum(QStringLiteral("S:vs"), QStringLiteral("Pearl Jam"),
                                          QStringLiteral("Versus"), 1993, 12) });
        QVector<MusicMerge::Source> srcs;
        srcs.push_back({ QString(), &local });
        srcs.push_back({ QStringLiteral("srv-a"), &srv });

        const MusicMerge::Merged before = MusicMerge::merge(srcs, QStringLiteral("local"));
        CHECK(before.albumInstances(QStringLiteral("L:ten")).size() == 2);   // merged by the rules
        CHECK(before.albumInstances(QStringLiteral("L:vs")).size() == 1);    // refused by the rules

        // "These are NOT the same album" — the direction that gives a hidden record back.
        setAlbumOverride(QStringLiteral("Pearl Jam"), QStringLiteral("Ten"),
                         QStringLiteral("Pearl Jam"), QStringLiteral("Ten"), false);
        const MusicMerge::Merged split = MusicMerge::merge(srcs, QStringLiteral("local"));
        CHECK(split.albumInstances(QStringLiteral("L:ten")).size() == 1);
        CHECK(albumIn(split.idx, QStringLiteral("S:ten")) != nullptr);

        // "This IS the same album as..." — the direction that cures a duplicate.
        setAlbumOverride(QStringLiteral("Pearl Jam"), QStringLiteral("Vs."),
                         QStringLiteral("Pearl Jam"), QStringLiteral("Versus"), true);
        const MusicMerge::Merged joined = MusicMerge::merge(srcs, QStringLiteral("local"));
        CHECK(joined.albumInstances(QStringLiteral("L:vs")).size() == 2);
        CHECK(albumIn(joined.idx, QStringLiteral("S:vs")) == nullptr);

        // Clearing both restores exactly the original verdicts.
        clearAlbumOverride(QStringLiteral("Pearl Jam"), QStringLiteral("Ten"),
                           QStringLiteral("Pearl Jam"), QStringLiteral("Ten"));
        clearAlbumOverride(QStringLiteral("Pearl Jam"), QStringLiteral("Vs."),
                           QStringLiteral("Pearl Jam"), QStringLiteral("Versus"));
        const MusicMerge::Merged after = MusicMerge::merge(srcs, QStringLiteral("local"));
        CHECK(after.albumInstances(QStringLiteral("L:ten")).size() == 2);
        CHECK(after.albumInstances(QStringLiteral("L:vs")).size() == 1);
    }

    // Leave nothing behind (issue #42).
    QFile::remove(ini);

    if (failures == 0) { std::puts("MUSICID-OK"); return 0; }
    std::fprintf(stderr, "MUSICID: %d check(s) failed\n", failures);
    return 1;
}
