// PLAYING A MERGED ALBUM WHEN ITS PREFERRED COPY CANNOT BE REACHED (issue #194, offline fallback) — the
// pure plan the album's Play press follows.
//
// A merged album is keyed on the copy the preference picked (MusicMerge.h), and it STAYS keyed there when
// that copy's server goes down. What changes is only which copy's files the press opens, and under what
// names those files are filed while they play. Both halves are decided here, from plain values, so
// probe_musicsources can hold every arm without a UI, a server or a store.
//
// ==================================================================================================
// IDENTITY AND HISTORY DO NOT MOVE
// ==================================================================================================
// A fallback play is filed under the MERGED record's own track identities — the preferred copy's tracks —
// not under the copy that happened to be reachable. So the resume position banked while streaming is where
// the local file resumes from, the listening seconds land on the row the user sees, and nothing has to be
// moved afterwards: the merged row's primary does not change, so MusicRemap::tableFor produces exactly the
// table it produced before the server went down, and the play does not trigger a remap.
//
// The match between the two copies' tracks is MusicRemap::tableFor's own (track number, then normalised
// title, anything ambiguous left out), applied to a group of exactly two: the preferred copy first, the
// copy that plays second. A track it cannot match keeps its own name — rule 1 of MusicRemap, and the same
// outcome as the preferred copy's track list not being fetched yet: nothing is guessed, and the ordinary
// merge remap moves such a record the next time both lists are known.
//
// ==================================================================================================
// A WHOLE ARTIST (issue #465): "Play all" / "Shuffle all" follow the SAME rule, album by album
// ==================================================================================================
// An artist queue spans records, and each record is a merged album with its own copies. There is ONE rule
// for which copy of a record plays, and it is plan() above: planArtist() runs it over every album of the
// artist and builds the queue from what it picked. So a record whose preferred server is down plays from
// this disk (or a download, or another server) exactly as its own "Play album" would, and a record with no
// copy that can be reached is SKIPPED — named in one sentence — rather than failing the whole queue.
//
// The track lists that choice needs are asked for by an ArtistPass: in parallel, a supplier that failed
// once in the pass is never asked again, and the whole pass has ONE cap (kArtistPassCapMs) rather than a
// per-album wait that would serialise into minutes on a long discography.
#pragma once
#include "MusicId.h"
#include "MusicQueue.h"
#include "MusicRemap.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace MusicFallback
{
    // One copy of a merged album. `sourceId` is "" for the local library; `onDisk` is "every track is a
    // downloaded file" for a server copy (a local copy is on disk by definition). `tracks` is empty when the
    // copy's track list has not been fetched.
    struct Copy
    {
        QString                       key;
        QString                       sourceId;
        bool                          onDisk = false;
        QVector<MusicRemap::TrackId>  tracks;
    };

    // How the reason line reads. Local and Downloaded are the "your own files" answers; Server is another
    // music server that is still answering.
    enum class Via { Preferred, Local, Downloaded, Server, Nothing };

    struct Plan
    {
        MusicId::PlayPick pick;
        Via               via = Via::Nothing;
        // Played track's index identity -> the preferred copy's track identity. Empty unless this is a
        // fallback. Handed to the player's identity table, never to a store.
        QHash<QString, QString> identities;
        bool fallback() const { return pick.fallback(); }
    };

    // `copies` PRIMARY FIRST — MusicMerge::Merged::albumInstances' own order — so the preferred copy is the
    // one the row is keyed on.
    inline Plan plan(const QVector<Copy>& copies, const QString& preference, const MusicId::Reachability& reach)
    {
        Plan out;
        QVector<MusicId::SourceRef> refs;
        refs.reserve(copies.size());
        for (const Copy& c : copies)
        {
            MusicId::SourceRef r;
            r.serverId = c.sourceId;
            r.onDisk   = c.onDisk;
            refs.push_back(r);
        }
        out.pick = MusicId::pickAutoSource(refs, preference, reach);
        if (out.pick.none()) { out.via = Via::Nothing; return out; }
        if (!out.pick.fallback()) { out.via = Via::Preferred; return out; }

        const Copy& played = copies.at(out.pick.index);
        out.via = played.sourceId.isEmpty() ? Via::Local : played.onDisk ? Via::Downloaded : Via::Server;

        MusicRemap::AlbumGroup g;
        MusicRemap::Instance pri, alt;
        pri.key = copies.at(out.pick.preferred).key; pri.tracks = copies.at(out.pick.preferred).tracks;
        alt.key = played.key;                        alt.tracks = played.tracks;
        g.instances = { pri, alt };
        out.identities = MusicRemap::tableFor({ g }).map;
        return out;
    }

    // The START row of a fallback play. A track row on the album page names the PREFERRED copy's track; the
    // queue is the other copy's, so the row is looked up through the identity table backwards. Unmatched ->
    // "" (from the top), which is what an album play with an unknown start already does.
    inline QString startFor(const Plan& p, const QString& preferredTrack)
    {
        if (preferredTrack.isEmpty() || !p.fallback()) return preferredTrack;
        for (auto it = p.identities.cbegin(); it != p.identities.cend(); ++it)
            if (it.value() == preferredTrack) return it.key();
        return QString();
    }

    // ==============================================================================================
    // A WHOLE ARTIST (#465)
    // ==============================================================================================

    // The ONE cap on everything an artist's Play all / Shuffle all waits for. A single album waits up to
    // 4 s for one answer (#194); an artist's questions go out together and the whole pass stops here, so a
    // discography of twenty records costs at most this, never twenty times the album's wait.
    constexpr int kArtistPassCapMs = 6000;

    // One record of the artist: the key its merged row is rendered under, its display title, and its copies
    // PRIMARY FIRST — exactly plan()'s input.
    struct ArtistAlbum
    {
        QString        key;
        QString        title;
        QVector<Copy>  copies;
    };

    // What one record contributes to the queue.
    struct ArtistPick
    {
        QString albumKey;   // the merged row's key
        QString playKey;    // the copy that plays: albumKey itself unless this record falls back
        Plan    plan;       // plan()'s own answer for the record, identities included
    };

    struct ArtistPlan
    {
        QVector<ArtistPick> play;          // in the artist's own record order
        QStringList         skipped;       // titles of the records that have nothing that can be played
        QStringList         downSources;   // distinct unreachable source ids behind a skip or a fallback
        int                 fallbacks = 0; // records playing a copy other than their preferred one

        // False when every record plays its preferred copy: the queue is then exactly today's, and the caller
        // builds it the way it always has.
        bool changed() const { return fallbacks > 0 || !skipped.isEmpty(); }
    };

    // THE ALBUM RULE, RECORD BY RECORD. Each record's copy is plan()'s pick — nothing here ranks copies of
    // its own. A record whose pick is nothing is SKIPPED and the queue goes on; so is a record whose picked
    // copy has no track list (it was asked for and did not come), because a queue cannot hold a record it
    // cannot name a single track of.
    inline ArtistPlan planArtist(const QVector<ArtistAlbum>& albums, const QString& preference,
                                 const MusicId::Reachability& reach)
    {
        ArtistPlan out;
        auto noteDown = [&out](const QString& id) {
            if (!id.isEmpty() && !out.downSources.contains(id)) out.downSources << id;
        };
        for (const ArtistAlbum& a : albums)
        {
            if (a.copies.isEmpty()) { out.skipped << a.title; continue; }
            const Plan p = plan(a.copies, preference, reach);
            const QString preferredSource =
                p.pick.preferred >= 0 ? a.copies.at(p.pick.preferred).sourceId : QString();
            if (p.pick.none()) { out.skipped << a.title; noteDown(preferredSource); continue; }
            const Copy& c = a.copies.at(p.pick.index);
            if (c.tracks.isEmpty()) { out.skipped << a.title; continue; }
            if (p.fallback()) { ++out.fallbacks; noteDown(preferredSource); }
            out.play.push_back(ArtistPick{ a.key, c.key, p });
        }
        return out;
    }

    // The queue a changed plan plays. `tracksOf(copy key)` is the caller's one record-to-entries builder
    // (MusicQueue::forAlbum over that copy's own supplier), so what "the record" means cannot depend on
    // whether it fell back. `identities` is every fallback record's played track -> the merged record's own
    // track: the names the player files resume, listening time and scrobbles under.
    struct ArtistQueue
    {
        QVector<MusicQueue::Entry> entries;
        QHash<QString, QString>    identities;
    };

    inline ArtistQueue artistQueue(const ArtistPlan& p,
                                   const std::function<QVector<MusicQueue::Entry>(const QString&)>& tracksOf)
    {
        ArtistQueue out;
        for (const ArtistPick& pick : p.play)
        {
            out.entries += tracksOf(pick.playKey);
            for (auto it = pick.plan.identities.cbegin(); it != pick.plan.identities.cend(); ++it)
                out.identities.insert(it.key(), it.value());
        }
        return out;
    }

    // The ONE sentence a changed plan says: "Skipped 2 albums (Navidrome is unreachable)". Empty when nothing
    // changed. `labelOf` turns a source id into the name the user gave it.
    inline QString artistSentence(const ArtistPlan& p, const std::function<QString(const QString&)>& labelOf)
    {
        if (!p.changed()) return QString();
        auto tr = [](const char* s, int n = -1) { return QCoreApplication::translate("MusicFallback", s, nullptr, n); };
        QStringList names;
        for (const QString& id : p.downSources) names << labelOf(id);
        const QString why = names.isEmpty()    ? QString()
                          : names.size() == 1 ? tr("%1 is unreachable").arg(names.first())
                                              : tr("%1 are unreachable").arg(names.join(QStringLiteral(", ")));
        if (!p.skipped.isEmpty())
        {
            const QString what = p.skipped.size() == 1 ? tr("Skipped \"%1\"").arg(p.skipped.first())
                                                       : tr("Skipped %n albums", int(p.skipped.size()));
            return tr("%1 (%2)").arg(what, why.isEmpty() ? tr("it could not be loaded", int(p.skipped.size()))
                                                         : why);
        }
        return tr("Playing %n album(s) from another copy (%1)", p.fallbacks).arg(why);
    }

    // One track-list question of a pass.
    struct Ask
    {
        QString key;        // the copy whose track list is asked for
        QString sourceId;   // its supplier
    };

    // THE QUESTIONS AN ARTIST QUEUE ASKS BEFORE IT PLAYS, and the three rules that bound them:
    //
    //   * PARALLEL. next() returns every question the current picks need, at once; the caller fires them
    //     together and calls next() again only when all have answered (idle()). A second round exists only
    //     because an answer can move a pick — the preferred server failed, so the record now falls back to
    //     another server whose list nobody has fetched.
    //   * A SUPPLIER THAT FAILED IN THIS PASS IS NOT ASKED AGAIN. It counts as unreachable for the rest of the
    //     pass, so no pick lands on it and no question goes to it. One that failed in an EARLIER pass is asked
    //     again — once — which is how a server that has come back is found on the very next press.
    //   * ONE CAP. At kArtistPassCapMs the caller calls expire(): every question still out counts as no
    //     answer, its supplier as failed, and nothing further is asked.
    //
    // What is asked: a remote copy a record would play from is asked for its track list when it has none;
    // otherwise its supplier is asked ONE question per pass (its first record's list), which is what finds
    // out that a server whose lists are cached has since stopped. A copy on this disk asks nothing, and with
    // no network at all (`offline`) nothing remote is picked, so nothing is asked.
    class ArtistPass
    {
    public:
        QVector<Ask> next(const QVector<ArtistAlbum>& albums, const QString& preference,
                          const MusicId::Reachability& base)
        {
            QVector<Ask> out;
            if (expired_) return out;
            // Planned as if every supplier not yet failed IN THIS PASS answers: that is what gets each one a
            // question before the queue relies on it.
            MusicId::Reachability r;
            r.offline     = base.offline;
            r.unreachable = failed_;
            for (const ArtistAlbum& a : albums)
            {
                if (a.copies.isEmpty()) continue;
                const Plan p = plan(a.copies, preference, r);
                if (p.pick.none()) continue;
                const Copy& c = a.copies.at(p.pick.index);
                if (c.sourceId.isEmpty() || c.onDisk) continue;          // a file on this disk needs no answer
                if (asked_.contains(c.key) || failed_.contains(c.sourceId)) continue;
                if (!c.tracks.isEmpty() && probed_.contains(c.sourceId)) continue;   // listed, and it was asked
                asked_.insert(c.key);
                probed_.insert(c.sourceId);
                outstanding_.insert(c.key, c.sourceId);
                out.push_back(Ask{ c.key, c.sourceId });
            }
            return out;
        }

        // One answer came back. `unreachable` is "the supplier did not answer" (MusicReach's verdict), not
        // "the answer was an error": a server that answers 404 is up, and its record is simply not playable.
        void answered(const QString& key, bool unreachable)
        {
            const auto it = outstanding_.find(key);
            if (it == outstanding_.end()) return;
            if (unreachable) failed_.insert(it.value());
            outstanding_.erase(it);
        }

        // The cap. Returns the suppliers that were still being waited for (each once), now failed.
        QStringList expire()
        {
            expired_ = true;
            QStringList down;
            for (auto it = outstanding_.cbegin(); it != outstanding_.cend(); ++it)
            {
                failed_.insert(it.value());
                if (!down.contains(it.value())) down << it.value();
            }
            outstanding_.clear();
            return down;
        }

        bool idle() const    { return outstanding_.isEmpty(); }
        bool expired() const { return expired_; }

        // The reachability the queue is planned with once the pass is over: every supplier that failed in it,
        // plus any the pass never got to ask (the cap struck first) that was already known to be down.
        MusicId::Reachability reach(const MusicId::Reachability& base) const
        {
            MusicId::Reachability r;
            r.offline     = base.offline;
            r.unreachable = failed_;
            for (const QString& id : base.unreachable)
                if (!probed_.contains(id)) r.unreachable.insert(id);
            return r;
        }

    private:
        QSet<QString>            asked_;        // copy keys asked in this pass
        QSet<QString>            probed_;       // suppliers asked at least once in this pass
        QSet<QString>            failed_;       // suppliers down for the rest of this pass
        QHash<QString, QString>  outstanding_;  // copy key -> supplier, awaiting an answer
        bool                     expired_ = false;
    };
}
