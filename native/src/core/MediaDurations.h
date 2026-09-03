// THE DURATION INDEX (issue #179, increment 1) — how long each item the app has actually opened turned out
// to be, kept as a durable per-item fact.
//
// Personal TV channels need lengths BEFORE anything plays: the day's timeline is laid out from them, and the
// issue's hard boundary is that a channel is built from items we can enumerate with known durations, never by
// probing files while a guide is being drawn. So the length has to be somewhere a schedule can read it, and
// until this existed it was not.
//
// IT IS NOT THE RESUME GROUP, and that is the whole reason it exists. `resume/<hash>/dur` already holds a
// length, but only while the item is UNFINISHED: PlaybackSession::finishResume deletes the whole group when a
// file plays to its end (issue #150, and rightly — a finished file has no position). So the one measurement
// the app ever takes of a video's length is thrown away by the act of watching it, and the items most likely
// to be in a channel — the ones you have seen — would be exactly the ones with no length. This store keeps
// the number after the position is gone.
//
// Layout — GLOBAL, not per profile, the same posture resume/* and metaoverrides/* take: how long a file is
// is a property of the file, not of who watched it.
//   mediadur/<hash>  ->  whole seconds
// <hash> is ResumeStore's hash of the item's stable key, reused verbatim (ResumeStore::groupFor) so the two
// stores are indexed the same way and a key that resumes is a key that has a length.
//
// DEVICE-LOCAL (CloudSync::isDeviceLocalKey). A duration is re-derived by opening the file, it says nothing
// about what the user did, and it would otherwise grow the synced settings bundle by a row per file ever
// opened — for a number the other device works out for itself the first time it plays anything.
#pragma once
#include <QString>

class QSettings;

namespace MediaDurations
{
    // The ini key one item's length lives under: "mediadur/<hash>".
    QString keyFor(const QString& itemKey);

    // Record a measured length. `seconds` <= 0 is IGNORED rather than stored — mpv reports 0 for a stream
    // whose container has no length, and a zero would be indistinguishable from "known to be instantaneous",
    // which would then divide a channel's day into nothing. A later, better measurement overwrites an
    // earlier one (the same file re-opened): the newest measurement is the one taken with the most of the
    // file available, which for a partially-downloaded item is strictly better information.
    void note(const QString& itemKey, int seconds);

    // The known length in whole seconds, or 0 for "not known". 0 is the ONE spelling of unknown; callers
    // (channels::withDurations) treat it as a skip, never as a length.
    int  seconds(const QString& itemKey);

    // The same two verbs against a caller-supplied QSettings — what a probe drives, and what lets this file
    // be tested without the app's shared ini. The app's own store is used by the two above.
    void noteIn(QSettings& s, const QString& itemKey, int seconds);
    int  secondsIn(QSettings& s, const QString& itemKey);
}
