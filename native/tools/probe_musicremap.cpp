// Headless check of THE MUSIC-IDENTITY REMAP (issue #194, increment 2) — the half of the cross-source merge
// that keeps what the user already banked when they change which source they prefer.
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
// "server" copy names them by a qualified track id, and its playId is a signed stream url — which is exactly
// why the two tables cannot be one (MusicRemap.h).
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

        CHECK(t.play.size() == 4);
        CHECK(t.index.size() == 4);
        for (int i = 0; i < 4; ++i)
        {
            CHECK(t.play.value(S.tracks.at(i).playId) == L.tracks.at(i).playId);
            CHECK(t.index.value(S.tracks.at(i).indexId) == L.tracks.at(i).indexId);
        }
        // ...and NOTHING in the other direction: the primary's own identities are never sources.
        for (int i = 0; i < 4; ++i)
        {
            CHECK(!t.play.contains(L.tracks.at(i).playId));
            CHECK(!t.index.contains(L.tracks.at(i).indexId));
        }
        // Purity: the same input twice gives the same table, and no key maps to an empty destination.
        const Table again = tableFor({ grp(L, S) });
        CHECK(again.play == t.play && again.index == t.index);
        for (auto it = t.play.cbegin(); it != t.play.cend(); ++it) CHECK(!it.value().isEmpty());
        for (auto it = t.index.cbegin(); it != t.index.cend(); ++it) CHECK(!it.value().isEmpty());
    }

    // =====================================================================================================
    // 2. THE PICK IS THE FIRST INSTANCE, WHICHEVER WAY ROUND — flipping the preference flips the table
    // =====================================================================================================
    {
        const Instance L = localCopy(QStringLiteral("local:okc"), kOk);
        const Instance S = serverCopy(QStringLiteral("sub|srv1|album|al1"), kOk);
        const Table toLocal  = tableFor({ grp(L, S) });
        const Table toServer = tableFor({ grp(S, L) });
        CHECK(toServer.play.value(L.tracks.at(0).playId) == S.tracks.at(0).playId);
        CHECK(toLocal.play.value(S.tracks.at(0).playId) == L.tracks.at(0).playId);
        // The two are exact inverses, which is what makes "change it back and everything comes home" true.
        CHECK(toServer.play.size() == toLocal.play.size());
        for (auto it = toLocal.play.cbegin(); it != toLocal.play.cend(); ++it)
            CHECK(toServer.play.value(it.value()) == it.key());
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
        CHECK(t.play.size() == 4);                                   // the four that exist on both
        CHECK(!t.play.contains(S.tracks.at(4).playId));              // the bonus track: no destination
        CHECK(!t.index.contains(S.tracks.at(4).indexId));
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
        CHECK(!t.play.contains(B.tracks.at(0).playId));
        CHECK(!t.index.contains(B.tracks.at(0).indexId));
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
        CHECK(t.play.value(S.tracks.at(1).playId) == L.tracks.at(1).playId);

        // No numbers anywhere (an untagged rip): the normalised title carries it, including through the
        // remaster noise MusicId::normalizeAlbum already strips.
        const Instance L2 = localCopy(QStringLiteral("local:b"),
                                      { QStringLiteral("Airbag"), QStringLiteral("Exit Music") }, false);
        const Instance S2 = serverCopy(QStringLiteral("sub|srv1|album|b"),
                                       { QStringLiteral("Exit  Music"), QStringLiteral("Áirbag") }, false);
        const Table t2 = tableFor({ grp(L2, S2) });
        CHECK(t2.play.value(S2.tracks.at(0).playId) == L2.tracks.at(1).playId);   // Exit Music -> Exit Music
        CHECK(t2.play.value(S2.tracks.at(1).playId) == L2.tracks.at(0).playId);   // Áirbag     -> Airbag

        // An unnameable track (empty title, no number) matches nothing, including another one like it.
        Instance L3 = localCopy(QStringLiteral("local:c"), { QString() }, false);
        Instance S3 = serverCopy(QStringLiteral("sub|srv1|album|c"), { QString() }, false);
        CHECK(tableFor({ grp(L3, S3) }).isEmpty());
    }

    // =====================================================================================================
    // 5b. A DESTINATION THAT CANNOT BE NAMED IS NOT A DESTINATION
    //
    // This is rule 1's sharpest edge and it is a REAL case, not a hypothetical: MusicSupply::playUrl returns
    // an EMPTY string when the server a qualified track belongs to is no longer configured (its credentials
    // are gone, so no stream url can be signed). The primary copy then offers a track with a real index id
    // and no play id at all. Mapping that identity to "" would send the record to the key md5("") — one
    // shared bucket, every migrated record on top of the last — which is the difference between "a few rows
    // left on old keys" and "the user's listening destroyed". The INDEX half still maps, because the
    // credential-free qualified id is still perfectly nameable.
    // =====================================================================================================
    {
        Instance P = serverCopy(QStringLiteral("sub|gone|album|al1"), kOk);
        for (int i = 0; i < P.tracks.size(); ++i) P.tracks[i].playId.clear();
        const Instance L0 = localCopy(QStringLiteral("local:okc"), kOk);
        const Table t = tableFor({ grp(P, L0) });
        CHECK(t.play.isEmpty());                 // nothing may be sent to an unnameable destination...
        CHECK(t.index.size() == 4);              // ...and the half that CAN be named still moves
        for (auto it = t.play.cbegin(); it != t.play.cend(); ++it) CHECK(!it.value().isEmpty());

        const QString bad = QDir::temp().filePath(QStringLiteral("eb-probe-musicremap-empty-dst.ini"));
        QFile::remove(bad);
        MusicRemap::setRemapIniPathForTesting(bad);
        {
            QSettings w(bad, QSettings::IniFormat);
            w.setValue(resumeGroup(L0.tracks.at(0).playId) + QStringLiteral("/pos"), 88.0);
            w.setValue(statsKey(QStringLiteral("default"), QStringLiteral("devA"), L0.tracks.at(0).playId),
                       statsBlob(700, 1000, QStringLiteral("Airbag")));
            w.sync();
        }
        applyRemap(t);
        {
            QSettings r(bad, QSettings::IniFormat);
            r.sync();
            CHECK(r.value(resumeGroup(L0.tracks.at(0).playId) + QStringLiteral("/pos")).toDouble() == 88.0);
            CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"),
                                           L0.tracks.at(0).playId)) == 700);
            // The one bucket every record would have collapsed into.
            CHECK(!r.contains(resumeGroup(QString()) + QStringLiteral("/pos")));
            CHECK(!r.contains(statsKey(QStringLiteral("default"), QStringLiteral("devA"), QString())));
        }
        QFile::remove(bad);
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
        w.setValue(resumeGroup(L.tracks.at(0).playId) + QStringLiteral("/pos"), 42.5);
        w.setValue(resumeGroup(L.tracks.at(0).playId) + QStringLiteral("/dur"), 300.0);
        w.setValue(resumeGroup(L.tracks.at(0).playId) + QStringLiteral("/ts"), 1000);
        w.setValue(resumeGroup(L.tracks.at(0).playId) + QStringLiteral("/title"), QStringLiteral("Airbag"));
        w.setValue(statsKey(QStringLiteral("default"), QStringLiteral("devA"), L.tracks.at(0).playId),
                   statsBlob(900, 1000, QStringLiteral("Airbag")));
        w.setValue(statsKey(QStringLiteral("kids"), QStringLiteral("devB"), L.tracks.at(1).playId),
                   statsBlob(120, 900, QStringLiteral("Paranoid Android")));
        w.setValue(statsLegacyKey(QStringLiteral("default"), L.tracks.at(2).playId),
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
        CHECK(!r.contains(resumeGroup(L.tracks.at(0).playId) + QStringLiteral("/pos")));
        CHECK(!r.contains(resumeGroup(L.tracks.at(0).playId) + QStringLiteral("/ts")));
        CHECK(r.value(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/pos")).toDouble() == 42.5);
        CHECK(r.value(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/dur")).toDouble() == 300.0);
        CHECK(r.value(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/ts")).toLongLong() == 1000);
        CHECK(r.value(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/title")).toString()
              == QStringLiteral("Airbag"));
        // ---- the seconds moved, in EVERY profile and EVERY device namespace, legacy shape included ------
        CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"),
                                       S.tracks.at(0).playId)) == 900);
        CHECK(!r.contains(statsKey(QStringLiteral("default"), QStringLiteral("devA"), L.tracks.at(0).playId)));
        CHECK(statsSeconds(r, statsKey(QStringLiteral("kids"), QStringLiteral("devB"),
                                       S.tracks.at(1).playId)) == 120);
        CHECK(statsSeconds(r, statsLegacyKey(QStringLiteral("default"), S.tracks.at(2).playId)) == 60);
        // ---- the index-keyed stores moved on the INDEX identity, not on the stream url ------------------
        CHECK(r.value(speedKey(S.tracks.at(1).indexId)).toString() == QStringLiteral("{\"rate\":1.25}"));
        CHECK(!r.contains(speedKey(L.tracks.at(1).indexId)));
        CHECK(r.value(syncKey(S.tracks.at(1).indexId, "audio")).toDouble() == 0.25);
        CHECK(r.value(syncKey(S.tracks.at(1).indexId, "sub")).toDouble() == -0.5);
        // ...and NOT under the play identity, which is the mistake one shared table would have made.
        CHECK(!r.contains(speedKey(S.tracks.at(1).playId)));
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
                                       S.tracks.at(0).playId)) == 900);   // NOT 1800, NOT 2700
        CHECK(r.value(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/pos")).toDouble() == 42.5);
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
        CHECK(r.value(resumeGroup(L.tracks.at(0).playId) + QStringLiteral("/pos")).toDouble() == 42.5);
        CHECK(!r.contains(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/pos")));
        CHECK(statsSeconds(r, statsKey(QStringLiteral("default"), QStringLiteral("devA"),
                                       L.tracks.at(0).playId)) == 900);
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
            w.setValue(statsKey(QStringLiteral("default"), QStringLiteral("devA"), L.tracks.at(0).playId),
                       statsBlob(900, 1000, QStringLiteral("Airbag")));
            w.setValue(statsKey(QStringLiteral("default"), QStringLiteral("devA"), S.tracks.at(0).playId),
                       statsBlob(300, 2000, QStringLiteral("Airbag (Remastered)")));
            // Two resume positions. A position is one point in one stream, so the NEWER one wins outright —
            // and the older record's leaves must not survive beside it.
            w.setValue(resumeGroup(L.tracks.at(0).playId) + QStringLiteral("/pos"), 42.5);
            w.setValue(resumeGroup(L.tracks.at(0).playId) + QStringLiteral("/dur"), 300.0);
            w.setValue(resumeGroup(L.tracks.at(0).playId) + QStringLiteral("/ts"), 3000);
            w.setValue(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/pos"), 10.0);
            w.setValue(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/dur"), 299.0);
            w.setValue(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/ts"), 2000);
            w.setValue(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/title"),
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
                                       S.tracks.at(0).playId)) == 1200);            // 900 + 300, not 900
        CHECK(!r.contains(statsKey(QStringLiteral("default"), QStringLiteral("devA"), L.tracks.at(0).playId)));
        CHECK(r.value(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/pos")).toDouble() == 42.5);
        CHECK(r.value(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/dur")).toDouble() == 300.0);
        // The loser's title leaf is CLEARED rather than left standing beside the winner's numbers — the
        // source carried no title, so neither does the destination.
        CHECK(!r.contains(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/title")));
        CHECK(r.value(speedKey(S.tracks.at(1).indexId)).toString() == QStringLiteral("{\"rate\":2.0}"));
        CHECK(!r.contains(speedKey(L.tracks.at(1).indexId)));
    }

    // ...and the OTHER direction of the same rule: an OLDER source must not overwrite a newer destination.
    {
        QFile::remove(ini);
        MusicRemap::setRemapIniPathForTesting(ini);
        {
            QSettings w(ini, QSettings::IniFormat);
            w.setValue(resumeGroup(L.tracks.at(0).playId) + QStringLiteral("/pos"), 5.0);
            w.setValue(resumeGroup(L.tracks.at(0).playId) + QStringLiteral("/ts"), 1000);
            w.setValue(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/pos"), 250.0);
            w.setValue(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/ts"), 5000);
            w.sync();
        }
        applyRemap(toServer);
        QSettings r(ini, QSettings::IniFormat);
        r.sync();
        CHECK(r.value(resumeGroup(S.tracks.at(0).playId) + QStringLiteral("/pos")).toDouble() == 250.0);
        CHECK(!r.contains(resumeGroup(L.tracks.at(0).playId) + QStringLiteral("/pos")));
    }

    // =====================================================================================================
    // 10. THE SINGLE-SOURCE INSTALL IS NOT TOUCHED AT ALL — no key read, no key written, no file created
    // =====================================================================================================
    {
        const QString fresh = QDir::temp().filePath(QStringLiteral("eb-probe-musicremap-empty.ini"));
        QFile::remove(fresh);
        MusicRemap::setRemapIniPathForTesting(fresh);
        AlbumGroup one; one.instances.push_back(localCopy(QStringLiteral("local:solo"), kOk));
        const Table t = tableFor({ one });
        CHECK(t.isEmpty());
        applyRemap(t);
        // applyRemap returns before it opens the store, so the ini is never even created.
        CHECK(!QFile::exists(fresh));
        QFile::remove(fresh);
    }

    QFile::remove(ini);
    if (failures == 0) std::printf("MUSICREMAP-OK\n");
    else               std::printf("MUSICREMAP-FAIL %d check(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
