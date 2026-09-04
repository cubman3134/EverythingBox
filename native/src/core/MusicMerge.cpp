#include "MusicMerge.h"

#include "MusicId.h"
#include "NaturalOrder.h"

#include <QCollator>
#include <QSet>
#include <algorithm>
#include <limits>
#include <utility>

namespace {

// The same comparator MusicLibrary::buildIndex sorts with (numeric-aware, case-insensitive), so a merged
// level and a single-source level order their rows by the same rule. A second, subtly different collator is
// exactly the drift that would make "the merge changed nothing" untrue for a library it never touched.
QCollator& naturalCollator()
{
    // Built by NaturalOrder for the reason MusicLibrary's is: an inline numeric-mode QCollator does nothing
    // at all under the C locale, and a merged level that orders differently from a single-source one is
    // exactly the drift this function exists to prevent (issue #205).
    static QCollator c = NaturalOrder::collator();
    return c;
}

// ---------------------------------------------------------------------------------------------------------
// Union-find with a per-group SOURCE SET. The set is what enforces "two instances from the same source never
// merge" on the UNION rather than only on the pair: without it, local A joined to server B joined to local C
// would put two local records in one group by transitivity, and one of them would vanish from the browse.
// ---------------------------------------------------------------------------------------------------------
class Groups
{
public:
    explicit Groups(const QStringList& sourceOfMember)
        : parent_(sourceOfMember.size()), sources_(sourceOfMember.size())
    {
        for (int i = 0; i < parent_.size(); ++i) { parent_[i] = i; sources_[i].insert(sourceOfMember.at(i)); }
    }

    int find(int x)
    {
        while (parent_[x] != x) { parent_[x] = parent_[parent_[x]]; x = parent_[x]; }
        return x;
    }

    // Returns false — and changes nothing — when the union would put two instances of one source together.
    bool join(int a, int b)
    {
        int ra = find(a), rb = find(b);
        if (ra == rb) return true;
        for (const QString& s : std::as_const(sources_[rb]))
            if (sources_[ra].contains(s)) return false;
        // The SMALLER index always becomes the root, so the grouping is a function of the input order alone.
        if (rb < ra) std::swap(ra, rb);
        parent_[rb] = ra;
        sources_[ra].unite(sources_[rb]);
        sources_[rb].clear();
        return true;
    }

private:
    QVector<int>        parent_;
    QVector<QSet<QString>> sources_;
};

// Try every pair inside every bucket. Buckets are small (one entry per source, in the ordinary case), and
// this is the ONLY place the predicate is evaluated — see MusicMerge.h on why the buckets are complete.
template <typename Pred>
void joinWithinBuckets(const QHash<QString, QVector<int>>& buckets, Groups& g, Pred sameFn)
{
    QStringList keys = buckets.keys();
    keys.sort();   // deterministic evaluation order, so the grouping does not depend on QHash iteration
    for (const QString& k : keys)
    {
        const QVector<int>& members = buckets.value(k);
        for (int i = 0; i < members.size(); ++i)
            for (int j = i + 1; j < members.size(); ++j)
                if (sameFn(members.at(i), members.at(j)))
                    g.join(members.at(i), members.at(j));
    }
}

// The user's "these ARE the same" verdicts, as extra candidate pairs across two different buckets.
template <typename Pred>
void joinAcrossOverrides(const QVector<MusicId::Verdict>& verdicts,
                         const QHash<QString, QVector<int>>& byOverrideKey, Groups& g, Pred sameFn)
{
    for (const MusicId::Verdict& v : verdicts)
    {
        if (!v.same) continue;
        const QVector<int> a = byOverrideKey.value(v.a);
        const QVector<int> b = byOverrideKey.value(v.b);
        for (int i : a)
            for (int j : b)
                if (i != j && sameFn(qMin(i, j), qMax(i, j)))
                    g.join(qMin(i, j), qMax(i, j));
    }
}

// Root -> its members, in the order the members were collected (source order, then that source's own order).
QVector<QVector<int>> componentsOf(Groups& g, int count)
{
    QHash<int, int>       slotOfRoot;
    QVector<QVector<int>> out;
    for (int i = 0; i < count; ++i)
    {
        const int r = g.find(i);
        auto it = slotOfRoot.constFind(r);
        if (it == slotOfRoot.constEnd()) { slotOfRoot.insert(r, int(out.size())); out.push_back({ i }); }
        else out[*it].push_back(i);
    }
    return out;
}

MusicId::AlbumFacts factsOf(const MusicLibrary::Album& b)
{
    MusicId::AlbumFacts f;
    f.albumArtist       = b.albumArtist;
    f.title             = b.title;
    f.artistMbid        = b.artistMbid;
    f.mbid.releaseGroup = b.mbidReleaseGroup;
    f.mbid.release      = b.mbidRelease;
    f.year              = b.year;
    f.trackCount        = b.trackCount;
    f.durationSec       = b.durationSec;
    return f;
}

} // namespace

QStringList MusicMerge::Merged::artistInstances(const QString& key) const
{
    const auto it = artistGroup.constFind(key);
    if (it != artistGroup.constEnd()) return *it;
    return key.isEmpty() ? QStringList() : QStringList{ key };
}

QStringList MusicMerge::Merged::albumInstances(const QString& key) const
{
    const auto it = albumGroup.constFind(key);
    if (it != albumGroup.constEnd()) return *it;
    return key.isEmpty() ? QStringList() : QStringList{ key };
}

MusicMerge::Merged MusicMerge::merge(const QVector<Source>& sources, const QString& preference)
{
    Merged out;

    // ---- The short-circuit. See the header: with one supplier the input comes back out untouched. --------
    QVector<const Source*> live;
    for (const Source& s : sources)
        if (s.index && !s.index->artists.isEmpty()) live.push_back(&s);
    if (live.isEmpty())
    {
        // Nothing has any content. Hand back the LOCAL supplier's (empty) index by preference, so that an
        // install whose scan has not finished still gets the very object #74's surfaces expect.
        for (const Source& s : sources) if (s.id.isEmpty() && s.index) { out.idx = *s.index; return out; }
        if (!sources.isEmpty() && sources.first().index) out.idx = *sources.first().index;
        return out;
    }
    if (live.size() == 1) { out.idx = *live.first()->index; return out; }

    out.active = true;

    // The local supplier's own track count carries over verbatim, because that is what "Shuffle all music"
    // actually queues (it runs over MusicLibrary::index(), not over this merge). A number here that counted
    // the servers' tracks too would promise a shuffle nothing can deliver.
    for (const Source* s : live)
        if (s->id.isEmpty()) out.idx.trackCount = s->index->trackCount;

    // ---- Artists -----------------------------------------------------------------------------------------
    struct ArtistInst { QString sourceId; const MusicLibrary::Artist* a = nullptr; };
    QVector<ArtistInst> arts;
    QStringList         artSource;
    for (const Source* s : live)
        for (const MusicLibrary::Artist& a : s->index->artists)
        { arts.push_back({ s->id, &a }); artSource << s->id; }

    Groups ag(artSource);
    {
        QHash<QString, QVector<int>> byName, byMbid;
        for (int i = 0; i < arts.size(); ++i)
        {
            const QString n = MusicId::normalizeArtist(arts.at(i).a->name);
            if (!n.isEmpty()) byName[n].push_back(i);
            const QString m = arts.at(i).a->mbid.trimmed().toCaseFolded();
            if (!m.isEmpty()) byMbid[m].push_back(i);
        }
        const auto same = [&arts](int i, int j) {
            return MusicId::sameArtist(arts.at(i).a->name, arts.at(i).a->mbid,
                                       arts.at(j).a->name, arts.at(j).a->mbid);
        };
        joinWithinBuckets(byName, ag, same);
        joinWithinBuckets(byMbid, ag, same);
        joinAcrossOverrides(MusicId::artistOverrides(), byName, ag, same);
    }

    const QVector<QVector<int>> artistGroups = componentsOf(ag, int(arts.size()));

    for (const QVector<int>& group : artistGroups)
    {
        // Which instance the merged row is keyed and played from.
        QVector<MusicId::SourceRef> refs;
        for (int i : group) refs.push_back({ arts.at(i).sourceId, true });
        const int pick = std::max(0, MusicId::pickAutoSource(refs, preference));
        const int primary = group.at(pick);

        MusicLibrary::Artist merged = *arts.at(primary).a;
        merged.albums.clear();

        // ---- That artist's albums, merged the same way ---------------------------------------------------
        struct AlbumInst { QString sourceId; const MusicLibrary::Album* b = nullptr; };
        QVector<AlbumInst> albums;
        QStringList        albSource;
        // Primary source first, then the rest in source order, so a tie in the album pick resolves the same
        // way the artist pick did.
        QVector<int> ordered{ primary };
        for (int i : group) if (i != primary) ordered.push_back(i);
        for (int i : ordered)
            for (const MusicLibrary::Album& b : arts.at(i).a->albums)
            { albums.push_back({ arts.at(i).sourceId, &b }); albSource << arts.at(i).sourceId; }

        Groups bg(albSource);
        {
            QHash<QString, QVector<int>> byTitle, byMbid, byOverrideKey;
            for (int i = 0; i < albums.size(); ++i)
            {
                const MusicLibrary::Album& b = *albums.at(i).b;
                // Inside one merged artist the artist half of the key is settled, so the bucket is the
                // normalised TITLE. The override store keys on artist+title, so that gets its own map.
                const QString t = MusicId::normalizeAlbum(b.title);
                if (!t.isEmpty()) byTitle[t].push_back(i);
                const QString rg = b.mbidReleaseGroup.trimmed().toCaseFolded();
                if (!rg.isEmpty()) byMbid[QStringLiteral("g:") + rg].push_back(i);
                const QString rel = b.mbidRelease.trimmed().toCaseFolded();
                if (!rel.isEmpty()) byMbid[QStringLiteral("r:") + rel].push_back(i);
                const QString ok = MusicId::albumKeyOf(b.albumArtist, b.title);
                if (!ok.isEmpty()) byOverrideKey[ok].push_back(i);
            }
            const auto same = [&albums](int i, int j) {
                return MusicId::sameAlbum(factsOf(*albums.at(i).b), factsOf(*albums.at(j).b));
            };
            joinWithinBuckets(byTitle, bg, same);
            joinWithinBuckets(byMbid, bg, same);
            joinAcrossOverrides(MusicId::albumOverrides(), byOverrideKey, bg, same);
        }

        const QVector<QVector<int>> albumGroups = componentsOf(bg, int(albums.size()));
        int knownAlbumCount = 0;
        for (int i : group) knownAlbumCount = std::max(knownAlbumCount, arts.at(i).a->albumCount);

        for (const QVector<int>& bgroup : albumGroups)
        {
            QVector<MusicId::SourceRef> brefs;
            for (int i : bgroup) brefs.push_back({ albums.at(i).sourceId, true });
            const int bpick = std::max(0, MusicId::pickAutoSource(brefs, preference));
            const int bprimary = bgroup.at(bpick);

            // VERBATIM. A merged album IS the picked instance — its key, its tracks, its cover folder, its
            // counts. Tracks stay per-source (the issue's own rule), so there is nothing to splice and
            // nothing that could silently play a file from the copy the user did not choose.
            const MusicLibrary::Album& chosen = *albums.at(bprimary).b;
            merged.albums.push_back(chosen);

            if (bgroup.size() > 1)
            {
                QStringList keys{ chosen.key };
                for (int i : bgroup) if (i != bprimary) keys << albums.at(i).b->key;
                out.albumGroup.insert(chosen.key, keys);
            }
            for (int i : bgroup) out.sourceOf.insert(albums.at(i).b->key, albums.at(i).sourceId);
        }

        std::sort(merged.albums.begin(), merged.albums.end(),
                  [](const MusicLibrary::Album& x, const MusicLibrary::Album& y) {
            const int xy = x.year > 0 ? x.year : std::numeric_limits<int>::max();
            const int yy = y.year > 0 ? y.year : std::numeric_limits<int>::max();
            if (xy != yy) return xy < yy;
            return naturalCollator().compare(x.title, y.title) < 0;
        });

        // The counts. `albumCount` is the larger of what we have merged and the largest count any instance
        // REPORTED, because a remote artist knows its album count long before its albums are fetched and a
        // row that said "0 albums" until you opened it would be a worse lie than an approximate total.
        //
        // `trackCount` IS THE MERGED DISCOGRAPHY'S, summed over the albums above (issue #194, increment 2).
        // It used to be the PRIMARY INSTANCE's, on the reasoning that "Play all" ran through
        // MusicSupply::indexFor(the primary key) and so could only ever queue one supplier's records. Both
        // halves of that have changed: the verb now queues THIS merged artist (MainWindow::openMusicQueue
        // takes the merged index when the merge is active), so a number counting one supplier's tracks would
        // understate what the row plays — and, worse, a remote primary reports 0, which withheld the verb
        // altogether. Each album contributes what it can honestly say: the tracks it holds, or the count its
        // server gave for tracks not fetched yet.
        merged.albumCount = std::max(int(merged.albums.size()), knownAlbumCount);
        merged.trackCount = 0;
        for (const MusicLibrary::Album& b : merged.albums)
            merged.trackCount += std::max(int(b.tracks.size()), b.trackCount);

        if (group.size() > 1)
        {
            QStringList keys{ merged.key };
            for (int i : group) if (i != primary) keys << arts.at(i).a->key;
            out.artistGroup.insert(merged.key, keys);
        }
        for (int i : group) out.sourceOf.insert(arts.at(i).a->key, arts.at(i).sourceId);

        out.idx.albumCount += int(merged.albums.size());
        out.idx.artists.push_back(merged);
    }

    // Artists alphabetically with the UNKNOWN bucket last — MusicLibrary::buildIndex's own rule, restated
    // here because a merged index is assembled rather than built and would otherwise come out in source order.
    std::sort(out.idx.artists.begin(), out.idx.artists.end(),
              [](const MusicLibrary::Artist& x, const MusicLibrary::Artist& y) {
        if (x.name.isEmpty() != y.name.isEmpty()) return y.name.isEmpty();
        return naturalCollator().compare(x.name, y.name) < 0;
    });

    // The classical view rides on the LOCAL library alone. A Subsonic server reports no composer tags at all
    // (the protocol's ID3 endpoints do not carry one), so there is nothing to merge and inventing an empty
    // second dimension would only cost the Composers door its "only when some file has one" gate.
    for (const Source* s : live)
        if (s->id.isEmpty()) out.idx.composers = s->index->composers;

    return out;
}

// ---- The picker's quality line (issue #194, increment 3) -----------------------------------------------
//
// See MusicMerge.h for why the rule lives here rather than in the surface that renders it, and why it
// refuses to guess.
QStringList MusicMerge::qualityBits(const MusicLibrary::Album& album)
{
    QStringList out;

    QString fmt = album.format.trimmed().toUpper();
    if (fmt.isEmpty() && !album.tracks.isEmpty())
    {
        // A LOCAL copy: the extension is exact, and it is what this level already showed. A remote key is
        // excluded BY SHAPE rather than by asking a protocol module — see the header.
        const QString sp = album.tracks.first().sourcePath;
        const bool looksLikeAPath = !sp.contains(QChar(0x1F))
                                    && (sp.contains(QLatin1Char('/')) || sp.contains(QLatin1Char('\\')));
        if (looksLikeAPath)
        {
            const int dot = sp.lastIndexOf(QLatin1Char('.'));
            // A dot must be inside the last few characters AND after the last separator, or a folder called
            // "The Wall (1979. Remaster)" would hand the picker a format of "REMASTER)".
            const int sep = qMax(sp.lastIndexOf(QLatin1Char('/')), sp.lastIndexOf(QLatin1Char('\\')));
            if (dot > sep && dot > 0 && sp.size() - dot <= 6) fmt = sp.mid(dot + 1).toUpper();
        }
    }
    if (!fmt.isEmpty()) out << fmt;
    if (album.bitrateKbps > 0) out << QString::number(album.bitrateKbps) + QStringLiteral(" kbps");
    return out;
}
