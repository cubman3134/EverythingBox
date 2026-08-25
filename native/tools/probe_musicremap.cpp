// Headless check of THE MUSIC-IDENTITY REMAP (issue #194, increment 2) — the half of the cross-source merge
// that keeps what the user already banked when they change which source they prefer — AND of the migration
// that gave a track ONE identity whichever route reached it (issue #204).
//
// #204 is why there is one table here rather than two. A Subsonic stream url is signed from the user's
// password, so keying the resume and consumption stores on it meant a password change silently orphaned
// every row the album route had written, and meant the same track banked twice when a playlist opened it
// instead. Sections 5b/5c/5d are that migration: the url is a SOURCE and never a name, the two buckets
// become one by a stated rule per store, and the two producers compose in a fixed order.
//
// The property that matters more than everything else here, and is pinned from BOTH sides:
//
//     A MIGRATION THAT SILENTLY LOSES A USER'S LISTENING IS FAR WORSE THAN ONE THAT LEAVES A FEW ROWS
//     BEHIND ON OLD KEYS.
//
// So every section that proves an unmappable identity is LEFT ALONE is paired with one that proves the
// mappable one still moves — because a remap biased to do nothing passes every "did not lose anything" test
// by being a no-op, and a no-op is the whole feature failing silently. native/tools/musicremap-mutants.json
// mutates in both directions for exactly that reason.
//
// Every key shape below is recomputed here from first principles (the digest, the truncation, the group
// path) rather than by calling MusicRemap's own helpers, so a drift between this file and the stores it
// mirrors shows up as a failing check instead of as a passing tautology.
//
// Prints MUSICREMAP-OK on success; any failure prints MUSICREMAP-FAIL <cond> and exits non-zero.
#include "MusicRemap.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "MUSICREMAP-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using namespace MusicRemap;

// ---------------------------------------------------------------------------------------------------------
// The key shapes, recomputed independently. See the header.
// ---------------------------------------------------------------------------------------------------------
static QString md5Full(const QString& s)
{
    return QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Md5).toHex());
}
static QString md5Ten(const QString& s) { return md5Full(s).left(10); }

static QString resumeGroup(const QString& playId) { return QStringLiteral("resume/") + md5Ten(playId); }
static QString statsKey(const QString& profile, const QString& device, const QString& playId)
{
    return QStringLiteral("stats/") + profile + QLatin1Char('/') + device
         + QStringLiteral("/items/") + md5Full(playId);
}
static QString statsLegacyKey(const QString& profile, const QString& playId)
{
    return QStringLiteral("stats/") + profile + QStringLiteral("/items/") + md5Full(playId);
}
static QString speedKey(const QString& indexId)
{
    return QStringLiteral("speed/items/") + md5Ten(indexId);
}
static QString syncKey(const QString& indexId, const char* axis)
{
    return QStringLiteral("sync/files/") + md5Full(indexId) + QLatin1Char('/') + QLatin1String(axis);
}

static QString statsBlob(qint64 seconds, qint64 lastActivity, const QString& title)
{
    QJsonObject o;
    o.insert(QStringLiteral("mediaSeconds"), double(seconds));
    o.insert(QStringLiteral("pagesRead"), 0.0);
    o.insert(QStringLiteral("lastActivity"), double(lastActivity));
    o.insert(QStringLiteral("title"), title);
    o.insert(QStringLiteral("category"), QStringLiteral("audio"));
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
static qint64 statsSeconds(QSettings& s, const QString& key)
{
    return qint64(QJsonDocument::fromJson(s.value(key).toString().toUtf8())
                      .object().value(QStringLiteral("mediaSeconds")).toDouble());
}

// ---------------------------------------------------------------------------------------------------------
// Fixture builders. A "local" copy names its tracks by file path (the same string is both identities); a
// "server" copy names them by a qualified track id, and its playId is a signed stream url — the string #204
// took out of every store's key, and which appears here only as the SOURCE of a migration (MusicRemap.h).
// The `t=` value is a fake token: nothing in this file is or ever was a real credential.
// ---------------------------------------------------------------------------------------------------------
static Instance localCopy(const QString& key, const QStringList& titles, bool numbered = true)
{
    Instance in;
    in.key = key;
    for (int i = 0; i < titles.size(); ++i)
    {
        TrackId t;
        t.number  = numbered ? (i + 1) : 0;
        t.title   = titles.at(i);
        t.playId  = key + QStringLiteral("/%1.flac").arg(i + 1);
        t.indexId = t.playId;      // a local track answers to ONE name
        in.tracks.push_back(t);
    }
    return in;
}

static Instance serverCopy(const QString& key, const QStringList& titles, bool numbered = true)
{
    Instance in;
    in.key = key;
    for (int i = 0; i < titles.size(); ++i)
    {
        TrackId t;
        t.number  = numbered ? (i + 1) : 0;
        t.title   = titles.at(i);
        t.indexId = key + QStringLiteral("|track|t%1").arg(i + 1);
        t.playId  = QStringLiteral("https://box.example/rest/stream.view?u=x&t=deadbeef&s=abc&id=t%1")
                        .arg(i + 1);
        in.tracks.push_back(t);
    }
    return in;
}

static AlbumGroup grp(const Instance& primary, const Instance& other)
{
    AlbumGroup g; g.instances.push_back(primary); g.instances.push_back(other); return g;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QStringList kOk{ QStringLiteral("Airbag"), QStringLiteral("Paranoid Android"),
                           QStringLiteral("Subterranean Homesick Alien"), QStringLiteral("Exit Music") };

    // =====================================================================================================
    // 1. THE TABLE IS PURE, AND IT MAPS EVERY TRACK OF A MERGED ALBUM ONTO THE PRIMARY COPY
    // =====================================================================================================
    {
        const Instance L = localCopy(QStringLiteral("local:okc"), kOk);
        const Instance S = serverCopy(QStringLiteral("sub|srv1|album|al1"), kOk);
        const Table t = tableFor({ grp(L, S) });

        CHECK(t.map.size() == 4);
        for (int i = 0; i < 4; ++i)
            CHECK(t.map.value(S.tracks.at(i).indexId) == L.tracks.at(i).indexId);
        // ...and NOTHING in the other direction: the primary's own identities are never sources.
        for (int i = 0; i < 4; ++i)
            CHECK(!t.map.contains(L.tracks.at(i).indexId));
        // #204: THE SIGNED STREAM URL IS NOT IN THIS TABLE AT ALL, in either column. A merge decision is
        // about which copy of a record a listener's history belongs to; it has no business touching the
        // credential-shaped half of a track's name, and before #204 it built a whole second table out of it.
        for (int i = 0; i < 4; ++i)
        {
            CHECK(!t.map.contains(S.tracks.at(i).playId));
            for (auto it = t.map.cbegin(); it != t.map.cend(); ++it)
                CHECK(it.value() != S.tracks.at(i).playId);
        }
        // Purity: the same input twice gives the same table, and no key maps to an empty destination.
        const Table again = tableFor({ grp(L, S) });
        CHECK(again.map == t.map);
        for (auto it = t.map.cbegin(); it != t.map.cend(); ++it) CHECK(!it.value().isEmpty());
    }

    // =====================================================================================================
    // 2. THE PICK IS THE FIRST INSTANCE, WHICHEVER WAY ROUND — flipping the preference flips the table
    // =====================================================================================================
    {
        const Instance L = localCopy(QStringLiteral("local:okc"), kOk);
        const Instance S = serverCopy(QStringLiteral("sub|srv1|album|al1"), kOk);
        const Table toLocal  = tableFor({ grp(L, S) });
        const Table toServer = tableFor({ grp(S, L) });
        CHECK(toServer.map.value(L.tracks.at(0).indexId) == S.tracks.at(0).indexId);
        CHECK(toLocal.map.value(S.tracks.at(0).indexId) == L.tracks.at(0).indexId);
        // The two are exact inverses, which is what makes "change it back and everything comes home" true.
        CHECK(toServer.map.size() == toLocal.map.size());
        for (auto it = toLocal.map.cbegin(); it != toLocal.map.cend(); ++it)
            CHECK(toServer.map.value(it.value()) == it.key());
    }

    // =====================================================================================================
    // 3. RULE 1: AN IDENTITY WITH NO DESTINATION IS ABSENT — never mapped to an empty string
    // =====================================================================================================
    {
        // (a) An album that is not merged at all.
        AlbumGroup alone; alone.instances.push_back(localCopy(QStringLiteral("local:kida"), kOk));
        CHECK(tableFor({ alone }).isEmpty());

        // (b) The PRIMARY copy's track list has not been fetched: there is no destination to name, so
        //     nothing moves and the other copy's records stay exactly where they are.
        Instance emptyPrimary; emptyPrimary.key = QStringLiteral("sub|srv1|album|al1");
        CHECK(tableFor({ grp(emptyPrimary, localCopy(QStringLiteral("local:okc"), kOk)) }).isEmpty());

        // (c) The OTHER copy's track list has not been fetched: nothing to move yet. It becomes movable the
        //     moment it is fetched, which is the whole reason this runs on every rebuild rather than once.
        Instance emptyOther; emptyOther.key = QStringLiteral("sub|srv1|album|al1");
        CHECK(tableFor({ grp(localCopy(QStringLiteral("local:okc"), kOk), emptyOther) }).isEmpty());

        // (d) A track that matches nothing on the other side keeps its own identity — and the tracks around
        //     it still move. A bonus track is the ordinary case here, and the album merged BECAUSE a
        //     differing track count is not a gate (MusicId).
        const Instance L = localCopy(QStringLiteral("local:okc"), kOk);
        Instance S = serverCopy(QStringLiteral("sub|srv1|album|al1"),
                                QStringList(kOk) << QStringLiteral("Lull (Bonus Track)"));
        const Table t = tableFor({ grp(L, S) });
        CHECK(t.map.size() == 4);                                    // the four that exist on both
        CHECK(!t.map.contains(S.tracks.at(4).indexId));              // the bonus track: no destination
    }

    // =====================================================================================================
    // 4. AMBIGUITY IS REFUSED, NOT ARBITRATED
    // =====================================================================================================
    {
        // Two discs numbered from 1 and titles that repeat: neither key is unique, so nothing is matched.
        const QStringList dup{ QStringLiteral("Intro"), QStringLiteral("Intro") };
        Instance L = localCopy(QStringLiteral("local:dup"), dup);
        Instance S = serverCopy(QStringLiteral("sub|srv1|album|dup"), dup);
        L.tracks[1].number = 1;  S.tracks[1].number = 1;             // both numbered 1
        CHECK(tableFor({ grp(L, S) }).isEmpty());

        // One identity, two different destinations (two groups claiming the same track) -> BANNED outright,
        // rather than resolved by whichever the hash iterated first.
        const Instance A = localCopy(QStringLiteral("local:one"), { QStringLiteral("Solo") });
        Instance B = serverCopy(QStringLiteral("sub|srv1|album|b"), { QStringLiteral("Solo") });
        Instance C = serverCopy(QStringLiteral("sub|srv1|album|c"), { QStringLiteral("Solo") });
        C.tracks[0].playId  = B.tracks.at(0).playId;                 // the same source identity...
        C.tracks[0].indexId = B.tracks.at(0).indexId;
        Instance A2 = A; A2.key = QStringLiteral("local:two");
        A2.tracks[0].playId  = QStringLiteral("local:two/1.flac");   // ...two different destinations
        A2.tracks[0].indexId = A2.tracks.at(0).playId;
        const Table t = tableFor({ grp(A, B), grp(A2, C) });
        CHECK(!t.map.contains(B.tracks.at(0).indexId));
    }

    // =====================================================================================================
    // 5. MATCHING: BY NUMBER FIRST, BY NORMALISED TITLE WHEN THERE IS NO NUMBER
    // =====================================================================================================
    {
        // Numbers agree, titles are spelled differently: the number carries it.
        const Instance L = localCopy(QStringLiteral("local:a"),
                                     { QStringLiteral("Airbag"), QStringLiteral("Paranoid Android") });
        const Instance S = serverCopy(QStringLiteral("sub|srv1|album|a"),
                                      { QStringLiteral("AIRBAG!!"), QStringLiteral("Karma Police") });
        const Table t = tableFor({ grp(L, S) });
        CHECK(t.map.value(S.tracks.at(1).indexId) == L.tracks.at(1).indexId);

        // No numbers anywhere (an untagged rip): the normalised title carries it, including through the
        // remaster noise MusicId::normalizeAlbum already strips.
        const Instance L2 = localCopy(QStringLiteral("local:b"),
                                      { QStringLiteral("Airbag"), QStringLiteral("Exit Music") }, false);
        const Instance S2 = serverCopy(QStringLiteral("sub|srv1|album|b"),
                                       { QStringLiteral("Exit  Music"), QStringLiteral("Áirbag") }, false);
        const Table t2 = tableFor({ grp(L2, S2) });
        CHECK(t2.map.value(S2.tracks.at(0).indexId) == L2.tracks.at(1).indexId);  // Exit Music -> Exit Music
        CHECK(t2.map.value(S2.tracks.at(1).indexId) == L2.tracks.at(0).indexId);  // Áirbag     -> Airbag

        // An unnameable track (empty title, no number) matches nothing, including another one like it.
        Instance L3 = localCopy(QStringLiteral("local:c"), { QString() }, false);
        Instance S3 = serverCopy(QStringLiteral("sub|srv1|album|c"), { QString() }, false);
        CHECK(tableFor({ grp(L3, S3) }).isEmpty());
    }

    // =====================================================================================================
    // 5b. THE #204 MIGRATION TABLE: A SIGNED URL IS A SOURCE, NEVER A NAME — AND AN EMPTY ONE IS NEITHER
    //
    // streamKeyTable is the whole of #204's data move: every track off the link it was played from and onto
    // the name it has always answered to. Rule 1's sharpest edge lives here in BOTH directions, and each
    // arm is a real case rather than a hypothetical:
    //
    //   * MusicSupply::playUrl returns an EMPTY string when the server a track belongs to is no longer
    //     configured — no credentials, so no url can be signed. md5("") is a perfectly real key that real
    //     rows can already be sitting under, so treating "" as a SOURCE would pick up some unrelated bucket
    //     and drop it on top of a track at random. Absent from the table.
    //   * An empty index id would send every record to that same one bucket as a DESTINATION — #194's
    //     `md5("")` hazard, unchanged. Absent too.
    //   * A LOCAL track's two names are one string, so it self-maps and is absent. That is what makes an
    //     install with no music server produce an empty table BY CONSTRUCTION.
    // =====================================================================================================
    {
        const Instance S0 = serverCopy(QStringLiteral("sub|srv1|album|al1"), kOk);
        const Instance L0 = localCopy(QStringLiteral("local:okc"), kOk);

        // The ordinary case: four server tracks, each mapped from its stream url onto its qualified id.
        const Table t = streamKeyTable(S0.tracks);
        CHECK(t.map.size() == 4);
        for (int i = 0; i < 4; ++i)
            CHECK(t.map.value(S0.tracks.at(i).playId) == S0.tracks.at(i).indexId);
        // A destination is never a url, and a source is never an id: the direction is the whole point.
        for (auto it = t.map.cbegin(); it != t.map.cend(); ++it)
        {
            CHECK(it.value().startsWith(QStringLiteral("sub|")));
            CHECK(it.key().startsWith(QStringLiteral("https://")));
        }
        // Purity, same as tableFor's.
        CHECK(streamKeyTable(S0.tracks).map == t.map);

        // A LOCAL LIBRARY PRODUCES NOTHING. No server, no signed url, no migration, no churn.
        CHECK(streamKeyTable(L0.tracks).isEmpty());

        // An unnameable url is not a source; an unnameable id is not a destination. Both drop out, and the
        // tracks around them still move.
        QVector<TrackId> mixed = S0.tracks;
        mixed[0].playId.clear();                                   // server gone: no url to name it by
        mixed[1].indexId.clear();                                  // no durable name to move it to
        const Table tm = streamKeyTable(mixed);
        CHECK(tm.map.size() == 2);
        CHECK(!tm.map.contains(QString()));
        for (auto it = tm.map.cbegin(); it != tm.map.cend(); ++it) CHECK(!it.value().isEmpty());
        CHECK(!tm.map.contains(S0.tracks.at(1).playId));
        CHECK(tm.map.value(S0.tracks.at(2).playId) == S0.tracks.at(2).indexId);

        // ONE URL, TWO TRACK IDS -> banned outright rather than arbitrated by hash order (rule 1).
        QVector<TrackId> clash = S0.tracks;
        clash[1].playId = clash.at(0).playId;
        CHECK(!streamKeyTable(clash).map.contains(clash.at(0).playId));

        // ...and at the STORE level: the md5("") bucket is never read and never written.
        const QString bad = QDir::temp().filePath(QStringLiteral("eb-probe-musicremap-empty-dst.ini"));
        QFile::remove(bad);
        MusicRemap::setRemapIniPathForTesting(bad);
        {
            QSettings w(bad, QSettings::IniFormat);
            // A row banked under md5("") — the bucket an empty-string source would sweep up — and a row on
            // the track whose server has gone away, which nothing can name any more.
            w.setValue(resumeGroup(QString()) + QStringLiteral("/pos"), 33.0);
            w.setValue(statsKey(QStringLiteral("default"), QStringLiteral("devA"), QString()),
                       statsBlob(500, 900, QStringLiteral("Orphan")));
            w.setValue(resumeGroup(L0.tracks.at(0).playId) + QStringLiteral("/pos"), 88.0);
            w.setValue(statsKey(QStringLiteral("default"), QStringLiteral("devA"), L0.tracks.at(0).playId),
                       statsBlob(700, 1000, QStringLiteral("Airbag")));
            w.sync();
        }
        applyRemap(tm);
        {
            QSettings r(bad, QSettings::IniFormat);
            r.sync();
            // The empty bucket stayed exactly where it was: not a source, not a destination.
            CHECK(r.value(resumeGroup(QString()) + QStringLiteral("/pos")).toDouble() == 33.0);
            CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"), QString()))
                  == 500);
            // The unnameable track's own row is untouched — nothing was named, so nothing moved (rule 1).
            CHECK(!r.contains(resumeGroup(S0.tracks.at(0).indexId) + QStringLiteral("/pos")));
            // And the local library is not in this table at all.
            CHECK(r.value(resumeGroup(L0.tracks.at(0).playId) + QStringLiteral("/pos")).toDouble() == 88.0);
            CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"),
                                           L0.tracks.at(0).playId)) == 700);
        }
        QFile::remove(bad);
    }

    // =====================================================================================================
    // 5c. THE HEADLINE OF #204, AT THE STORE LEVEL: TWO BUCKETS BECOME ONE, AND A PASSWORD CHANGE STOPS
    //     COSTING ANYTHING
    //
    // A track played from a PLAYLIST banked under its qualified id (#203); the same track played from the
    // ALBUM view banked under the signed stream url. Both halves are real listening and neither may be lost.
    // The merge rule per store is decided here rather than left to whichever pass ran last:
    //   * SECONDS SUM — additive, and the listener heard both.
    //   * A RESUME POSITION: the NEWER `ts` wins outright. Not the larger position: restarting a track you
    //     were 90% through must not throw you back to 90%.
    //   * A SPEED IS A SETTING: the destination's own wins.
    // Then the fact the issue is named after: re-sign the urls (which is what changing the password does)
    // and the position is still there, because nothing is keyed on a url any more.
    // =====================================================================================================
    {
        const Instance S0 = serverCopy(QStringLiteral("sub|srv1|album|al1"), kOk);
        const QString twoBuckets = QDir::temp().filePath(QStringLiteral("eb-probe-musicremap-buckets.ini"));
        QFile::remove(twoBuckets);
        MusicRemap::setRemapIniPathForTesting(twoBuckets);
        const TrackId& tr = S0.tracks.at(0);
        {
            QSettings w(twoBuckets, QSettings::IniFormat);
            // The ALBUM route's bucket: 600 seconds and an OLD position.
            w.setValue(statsKey(QStringLiteral("default"), QStringLiteral("devA"), tr.playId),
                       statsBlob(600, 1000, QStringLiteral("Airbag")));
            w.setValue(resumeGroup(tr.playId) + QStringLiteral("/pos"), 240.0);
            w.setValue(resumeGroup(tr.playId) + QStringLiteral("/dur"), 300.0);
            w.setValue(resumeGroup(tr.playId) + QStringLiteral("/ts"), 1000);
            w.setValue(speedKey(tr.playId), QStringLiteral("{\"rate\":2.0}"));
            // The PLAYLIST route's bucket: 90 seconds and a NEWER position — the user began it again.
            w.setValue(statsKey(QStringLiteral("default"), QStringLiteral("devA"), tr.indexId),
                       statsBlob(90, 5000, QStringLiteral("Airbag")));
            w.setValue(resumeGroup(tr.indexId) + QStringLiteral("/pos"), 12.0);
            w.setValue(resumeGroup(tr.indexId) + QStringLiteral("/dur"), 300.0);
            w.setValue(resumeGroup(tr.indexId) + QStringLiteral("/ts"), 5000);
            w.setValue(speedKey(tr.indexId), QStringLiteral("{\"rate\":1.0}"));
            w.sync();
        }
        applyRemap(streamKeyTable(S0.tracks));
        {
            QSettings r(twoBuckets, QSettings::IniFormat);
            r.sync();
            CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"), tr.indexId))
                  == 690);                                          // 600 + 90: neither half discarded
            CHECK(!r.contains(statsKey(QStringLiteral("default"), QStringLiteral("devA"), tr.playId)));
            CHECK(r.value(resumeGroup(tr.indexId) + QStringLiteral("/pos")).toDouble() == 12.0);   // NOT 240
            CHECK(r.value(resumeGroup(tr.indexId) + QStringLiteral("/ts")).toLongLong() == 5000);
            CHECK(!r.contains(resumeGroup(tr.playId) + QStringLiteral("/pos")));
            CHECK(r.value(speedKey(tr.indexId)).toString() == QStringLiteral("{\"rate\":1.0}"));
        }

        // RUN IT AGAIN: the sum must not become 1290, and the position must not move.
        applyRemap(streamKeyTable(S0.tracks));
        {
            QSettings r(twoBuckets, QSettings::IniFormat);
            r.sync();
            CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"), tr.indexId))
                  == 690);
            CHECK(r.value(resumeGroup(tr.indexId) + QStringLiteral("/pos")).toDouble() == 12.0);
        }

        // THE PASSWORD CHANGES. Every url this server signs is different from now on, so the OLD table's
        // sources name nothing — which is exactly the state that used to orphan the history. The row is
        // already off the url, so a table built from the NEW urls finds nothing to move and the position is
        // still exactly where the listener left it.
        Instance resigned = S0;
        for (int i = 0; i < resigned.tracks.size(); ++i)
            resigned.tracks[i].playId.replace(QStringLiteral("t=deadbeef"), QStringLiteral("t=cafef00d"));
        CHECK(resigned.tracks.at(0).playId != tr.playId);           // the fixture really did re-sign
        applyRemap(streamKeyTable(resigned.tracks));
        {
            QSettings r(twoBuckets, QSettings::IniFormat);
            r.sync();
            CHECK(r.value(resumeGroup(tr.indexId) + QStringLiteral("/pos")).toDouble() == 12.0);
            CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"), tr.indexId))
                  == 690);
            CHECK(!r.contains(resumeGroup(resigned.tracks.at(0).playId) + QStringLiteral("/pos")));
        }
        QFile::remove(twoBuckets);
    }

    // =====================================================================================================
    // 5d. THE TWO PRODUCERS COMPOSE, IN THAT ORDER, AND THE COMPOSITION IS IDEMPOTENT
    //
    // A destination of streamKeyTable is a source of tableFor, which is why they are two separate calls
    // rather than one merged hash: a single pass over a table holding both A->B and B->C would resolve them
    // in whatever order the hash iterated. Run in order, a record banked under a stream url before this
    // build existed ends up under the identity of the copy the user's source preference picked.
    // =====================================================================================================
    {
        const Instance S0 = serverCopy(QStringLiteral("sub|srv1|album|al1"), kOk);
        const Instance L0 = localCopy(QStringLiteral("local:okc"), kOk);
        const QString chain = QDir::temp().filePath(QStringLiteral("eb-probe-musicremap-chain.ini"));
        QFile::remove(chain);
        MusicRemap::setRemapIniPathForTesting(chain);
        {
            QSettings w(chain, QSettings::IniFormat);
            w.setValue(statsKey(QStringLiteral("default"), QStringLiteral("devA"),
                                S0.tracks.at(0).playId), statsBlob(300, 1000, QStringLiteral("Airbag")));
            w.setValue(resumeGroup(S0.tracks.at(0).playId) + QStringLiteral("/pos"), 55.0);
            w.setValue(resumeGroup(S0.tracks.at(0).playId) + QStringLiteral("/ts"), 1000);
            w.sync();
        }
        for (int pass = 0; pass < 2; ++pass)                        // twice: the composition is idempotent
        {
            applyRemap(streamKeyTable(S0.tracks));                  // url  -> the server track's own id
            applyRemap(tableFor({ grp(L0, S0) }));                  // that -> the LOCAL copy, the pick
        }
        {
            QSettings r(chain, QSettings::IniFormat);
            r.sync();
            CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"),
                                           L0.tracks.at(0).indexId)) == 300);      // NOT 600
            CHECK(r.value(resumeGroup(L0.tracks.at(0).indexId) + QStringLiteral("/pos")).toDouble() == 55.0);
            CHECK(!r.contains(resumeGroup(S0.tracks.at(0).playId) + QStringLiteral("/pos")));
            CHECK(!r.contains(resumeGroup(S0.tracks.at(0).indexId) + QStringLiteral("/pos")));
        }
        QFile::remove(chain);
    }

    // =====================================================================================================
    // 6. THE STORES. From here on the ini is real (redirected by the test seam) and every key is checked by
    //    its own recomputed shape.
    // =====================================================================================================
    const QString ini = QDir::temp().filePath(QStringLiteral("eb-probe-musicremap.ini"));
    QFile::remove(ini);
    MusicRemap::setRemapIniPathForTesting(ini);

    const Instance L = localCopy(QStringLiteral("local:okc"), kOk);
    const Instance S = serverCopy(QStringLiteral("sub|srv1|album|al1"), kOk);
    const Table toServer = tableFor({ grp(S, L) });   // the user has just switched to "a music server"

    {
        QSettings w(ini, QSettings::IniFormat);
        // A resume position and 900 accrued seconds on the LOCAL copy of track 1, under two profiles and two
        // device namespaces plus the pre-namespacing legacy shape.
        w.setValue(resumeGroup(L.tracks.at(0).indexId) + QStringLiteral("/pos"), 42.5);
        w.setValue(resumeGroup(L.tracks.at(0).indexId) + QStringLiteral("/dur"), 300.0);
        w.setValue(resumeGroup(L.tracks.at(0).indexId) + QStringLiteral("/ts"), 1000);
        w.setValue(resumeGroup(L.tracks.at(0).indexId) + QStringLiteral("/title"), QStringLiteral("Airbag"));
        w.setValue(statsKey(QStringLiteral("default"), QStringLiteral("devA"), L.tracks.at(0).indexId),
                   statsBlob(900, 1000, QStringLiteral("Airbag")));
        w.setValue(statsKey(QStringLiteral("kids"), QStringLiteral("devB"), L.tracks.at(1).indexId),
                   statsBlob(120, 900, QStringLiteral("Paranoid Android")));
        w.setValue(statsLegacyKey(QStringLiteral("default"), L.tracks.at(2).indexId),
                   statsBlob(60, 800, QStringLiteral("Subterranean")));
        // A playback speed and a pair of sync offsets on the INDEX identity of track 2.
        w.setValue(speedKey(L.tracks.at(1).indexId), QStringLiteral("{\"rate\":1.25}"));
        w.setValue(syncKey(L.tracks.at(1).indexId, "audio"), 0.25);
        w.setValue(syncKey(L.tracks.at(1).indexId, "sub"), -0.5);
        // A record under a DIFFERENT album entirely, which the remap must never touch.
        w.setValue(resumeGroup(QStringLiteral("D:/other/song.flac")) + QStringLiteral("/pos"), 7.0);
        w.setValue(statsKey(QStringLiteral("default"), QStringLiteral("devA"),
                            QStringLiteral("D:/other/song.flac")), statsBlob(50, 700, QStringLiteral("Other")));
        // The offline SCROBBLE queue, byte-for-byte. It holds no music key at all and must come out
        // identical: a remap that duplicated, dropped or re-dated a pending listen would corrupt a history
        // on a third-party service the user cannot easily undo.
        w.setValue(QStringLiteral("scrobblestate/default/listenbrainz/queue"),
                   QStringLiteral("[{\"a\":\"Radiohead\",\"t\":\"Airbag\",\"ts\":1699999999}]"));
        w.setValue(QStringLiteral("scrobblestate/default/listenbrainz/delivered"), 412);
        // A RECENT, which is also deliberately untouched — see the report: a local album's recent identity
        // is a track path, and rewriting it into a remote TRACK id would give the re-open route a string it
        // cannot resolve to an album.
        w.setValue(QStringLiteral("recent/default/items"),
                   QStringLiteral("[{\"path\":\"local:okc/1.flac\",\"kind\":\"audio\"}]"));
        w.sync();
    }

    applyRemap(toServer);

    {
        QSettings r(ini, QSettings::IniFormat);
        r.sync();
        // ---- the resume record moved, wholesale, and the source is gone ---------------------------------
        CHECK(!r.contains(resumeGroup(L.tracks.at(0).indexId) + QStringLiteral("/pos")));
        CHECK(!r.contains(resumeGroup(L.tracks.at(0).indexId) + QStringLiteral("/ts")));
        CHECK(r.value(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/pos")).toDouble() == 42.5);
        CHECK(r.value(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/dur")).toDouble() == 300.0);
        CHECK(r.value(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/ts")).toLongLong() == 1000);
        CHECK(r.value(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/title")).toString()
              == QStringLiteral("Airbag"));
        // ---- the seconds moved, in EVERY profile and EVERY device namespace, legacy shape included ------
        CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"),
                                       S.tracks.at(0).indexId)) == 900);
        CHECK(!r.contains(statsKey(QStringLiteral("default"), QStringLiteral("devA"), L.tracks.at(0).indexId)));
        CHECK(statsSeconds(r, statsKey(QStringLiteral("kids"), QStringLiteral("devB"),
                                       S.tracks.at(1).indexId)) == 120);
        CHECK(statsSeconds(r, statsLegacyKey(QStringLiteral("default"), S.tracks.at(2).indexId)) == 60);
        // ---- the index-keyed stores moved on the INDEX identity, not on the stream url ------------------
        CHECK(r.value(speedKey(S.tracks.at(1).indexId)).toString() == QStringLiteral("{\"rate\":1.25}"));
        CHECK(!r.contains(speedKey(L.tracks.at(1).indexId)));
        CHECK(r.value(syncKey(S.tracks.at(1).indexId, "audio")).toDouble() == 0.25);
        CHECK(r.value(syncKey(S.tracks.at(1).indexId, "sub")).toDouble() == -0.5);
        // #204: THE SIGNED URL IS NOT A KEY IN ANY OF THE FOUR STORES. Not after the move and not before it —
        // the merge table never names one, so nothing this pass writes can be reached only while the user's
        // password stays the same. Checked over all four shapes rather than one, because the bug this issue
        // is about was exactly one store keying differently from the rest.
        for (int i = 0; i < 4; ++i)
        {
            CHECK(!r.contains(resumeGroup(S.tracks.at(i).playId) + QStringLiteral("/pos")));
            CHECK(!r.contains(statsKey(QStringLiteral("default"), QStringLiteral("devA"),
                                       S.tracks.at(i).playId)));
            CHECK(!r.contains(speedKey(S.tracks.at(i).playId)));
            CHECK(!r.contains(syncKey(S.tracks.at(i).playId, "audio")));
        }
        // ---- everything that is not this album is untouched ----------------------------------------------
        CHECK(r.value(resumeGroup(QStringLiteral("D:/other/song.flac"))
                      + QStringLiteral("/pos")).toDouble() == 7.0);
        CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"),
                                       QStringLiteral("D:/other/song.flac"))) == 50);
        // ---- THE SCROBBLE QUEUE, BYTE FOR BYTE ----------------------------------------------------------
        CHECK(r.value(QStringLiteral("scrobblestate/default/listenbrainz/queue")).toString()
              == QStringLiteral("[{\"a\":\"Radiohead\",\"t\":\"Airbag\",\"ts\":1699999999}]"));
        CHECK(r.value(QStringLiteral("scrobblestate/default/listenbrainz/delivered")).toInt() == 412);
        CHECK(r.value(QStringLiteral("recent/default/items")).toString()
              == QStringLiteral("[{\"path\":\"local:okc/1.flac\",\"kind\":\"audio\"}]"));
    }

    // =====================================================================================================
    // 7. RULE 4: RUNNING IT TWICE EQUALS RUNNING IT ONCE
    // =====================================================================================================
    {
        applyRemap(toServer);
        applyRemap(toServer);
        QSettings r(ini, QSettings::IniFormat);
        r.sync();
        CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"),
                                       S.tracks.at(0).indexId)) == 900);   // NOT 1800, NOT 2700
        CHECK(r.value(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/pos")).toDouble() == 42.5);
        CHECK(r.value(speedKey(S.tracks.at(1).indexId)).toString() == QStringLiteral("{\"rate\":1.25}"));
    }

    // =====================================================================================================
    // 8. AND BACK AGAIN. The preference is a setting the user can change twice, so the records must come
    //    home — which is the case a one-shot stamped migration gets wrong.
    // =====================================================================================================
    {
        applyRemap(tableFor({ grp(L, S) }));
        QSettings r(ini, QSettings::IniFormat);
        r.sync();
        CHECK(r.value(resumeGroup(L.tracks.at(0).indexId) + QStringLiteral("/pos")).toDouble() == 42.5);
        CHECK(!r.contains(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/pos")));
        CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"),
                                       L.tracks.at(0).indexId)) == 900);
        CHECK(r.value(speedKey(L.tracks.at(1).indexId)).toString() == QStringLiteral("{\"rate\":1.25}"));
    }

    // =====================================================================================================
    // 9. RULE 3: A DESTINATION THAT ALREADY HOLDS A RECORD IS MERGED INTO, NEVER OVERWRITTEN
    // =====================================================================================================
    {
        QFile::remove(ini);
        MusicRemap::setRemapIniPathForTesting(ini);
        {
            QSettings w(ini, QSettings::IniFormat);
            // Both copies have been played: 900s here, 300s there. Neither may be discarded.
            w.setValue(statsKey(QStringLiteral("default"), QStringLiteral("devA"), L.tracks.at(0).indexId),
                       statsBlob(900, 1000, QStringLiteral("Airbag")));
            w.setValue(statsKey(QStringLiteral("default"), QStringLiteral("devA"), S.tracks.at(0).indexId),
                       statsBlob(300, 2000, QStringLiteral("Airbag (Remastered)")));
            // Two resume positions. A position is one point in one stream, so the NEWER one wins outright —
            // and the older record's leaves must not survive beside it.
            w.setValue(resumeGroup(L.tracks.at(0).indexId) + QStringLiteral("/pos"), 42.5);
            w.setValue(resumeGroup(L.tracks.at(0).indexId) + QStringLiteral("/dur"), 300.0);
            w.setValue(resumeGroup(L.tracks.at(0).indexId) + QStringLiteral("/ts"), 3000);
            w.setValue(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/pos"), 10.0);
            w.setValue(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/dur"), 299.0);
            w.setValue(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/ts"), 2000);
            w.setValue(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/title"),
                       QStringLiteral("stale"));
            // A speed on BOTH: the destination's own is the one the user set for the row now on screen.
            w.setValue(speedKey(L.tracks.at(1).indexId), QStringLiteral("{\"rate\":1.25}"));
            w.setValue(speedKey(S.tracks.at(1).indexId), QStringLiteral("{\"rate\":2.0}"));
            w.sync();
        }
        applyRemap(toServer);
        QSettings r(ini, QSettings::IniFormat);
        r.sync();
        CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"),
                                       S.tracks.at(0).indexId)) == 1200);            // 900 + 300, not 900
        CHECK(!r.contains(statsKey(QStringLiteral("default"), QStringLiteral("devA"), L.tracks.at(0).indexId)));
        CHECK(r.value(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/pos")).toDouble() == 42.5);
        CHECK(r.value(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/dur")).toDouble() == 300.0);
        // The loser's title leaf is CLEARED rather than left standing beside the winner's numbers — the
        // source carried no title, so neither does the destination.
        CHECK(!r.contains(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/title")));
        CHECK(r.value(speedKey(S.tracks.at(1).indexId)).toString() == QStringLiteral("{\"rate\":2.0}"));
        CHECK(!r.contains(speedKey(L.tracks.at(1).indexId)));
    }

    // ...and the OTHER direction of the same rule: an OLDER source must not overwrite a newer destination.
    {
        QFile::remove(ini);
        MusicRemap::setRemapIniPathForTesting(ini);
        {
            QSettings w(ini, QSettings::IniFormat);
            w.setValue(resumeGroup(L.tracks.at(0).indexId) + QStringLiteral("/pos"), 5.0);
            w.setValue(resumeGroup(L.tracks.at(0).indexId) + QStringLiteral("/ts"), 1000);
            w.setValue(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/pos"), 250.0);
            w.setValue(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/ts"), 5000);
            w.sync();
        }
        applyRemap(toServer);
        QSettings r(ini, QSettings::IniFormat);
        r.sync();
        CHECK(r.value(resumeGroup(S.tracks.at(0).indexId) + QStringLiteral("/pos")).toDouble() == 250.0);
        CHECK(!r.contains(resumeGroup(L.tracks.at(0).indexId) + QStringLiteral("/pos")));
    }

    // =====================================================================================================
    // 10. THE SINGLE-SOURCE INSTALL IS NOT TOUCHED AT ALL — no key read, no key written, no file created
    // =====================================================================================================
    {
        const QString fresh = QDir::temp().filePath(QStringLiteral("eb-probe-musicremap-empty.ini"));
        QFile::remove(fresh);
        MusicRemap::setRemapIniPathForTesting(fresh);
        const Instance solo = localCopy(QStringLiteral("local:solo"), kOk);
        AlbumGroup one; one.instances.push_back(solo);
        const Table t = tableFor({ one });
        CHECK(t.isEmpty());
        applyRemap(t);
        // #204: and the STREAM RE-KEY is just as inert on it. A local track's two names are one string, so
        // every offer is a self-map and the table comes out empty — which is what makes "an install with no
        // music server sees no migration churn" a fact about control flow rather than a claim.
        const Table s = streamKeyTable(solo.tracks);
        CHECK(s.isEmpty());
        applyRemap(s);
        // applyRemap returns before it opens the store, so the ini is never even created.
        CHECK(!QFile::exists(fresh));
        QFile::remove(fresh);
    }

    QFile::remove(ini);
    if (failures == 0) std::printf("MUSICREMAP-OK\n");
    else               std::printf("MUSICREMAP-FAIL %d check(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
