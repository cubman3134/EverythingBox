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
#pragma once
#include "MusicId.h"
#include "MusicRemap.h"

#include <QHash>
#include <QString>
#include <QVector>

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
}
