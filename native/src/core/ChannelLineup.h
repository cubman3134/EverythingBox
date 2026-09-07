// FROM A CHANNEL'S SOURCE TO ITS CANDIDATES (issue #179, increment 1) — the one impure half of the channel
// machinery: it reads the stores (PlaylistStore, LocalLibrary, the duration index) and hands
// channels::withDurations a plain candidate list. Everything downstream of here is pure.
//
// THE SPLIT IS THE POINT. `candidatesFor` is the only function in the feature that touches a store, and the
// duration lookup it pairs with is an argument rather than a call — so probe_channels drives every rule of
// the lineup (the duration gate, the skip list, the ordering, the whole day) with hand-built inputs and a
// hand-built clock, and nothing in the schedule can quietly grow a dependency on the app's live state.
//
// SOURCES IMPLEMENTED THIS INCREMENT: Playlist and LocalFolder. FilterPreset is stored, edited and
// round-tripped like the others, but resolving it means evaluating a #63 game filter across the WHOLE game
// library — an enumeration this app builds per browse level, not globally — so its candidates come from an
// injected resolver (`setPresetResolver`) that increment 1 does not install. A preset channel therefore
// builds an empty lineup and says so once in the log, which is the same shape a source with no
// duration-carrying items gets. AddonCatalog/ServerItems (increment 2) plug into the same seam.
#pragma once
#include "Channels.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace ChannelLineup
{
    // The items a channel's source names, BEFORE the duration gate. Reads the stores; no network, no file
    // probing, no duration lookup.
    QVector<channels::Candidate> candidatesFor(const channels::Channel& ch);

    // The duration index this app has: MediaDurations (the measured length of anything ever opened). Passed
    // to channels::withDurations as its lookup. NEVER opens the file — the whole gate exists so that a guide
    // draw costs no I/O.
    //
    // IT TRIES THE ITEM ID FIRST AND THE PLAY KEY SECOND, because the index is keyed by whatever identity
    // PlaybackSession resumed the item under, and that is the ITEM ID whenever it has one ("local:<path>" for
    // a local video, an addon item id for a catalogue row) and the path only when it does not. A live drive
    // found this: asking with the path alone made every already-watched file look length-less, which is the
    // one thing that cannot be told apart from a channel with nothing to air.
    int knownDurationSec(const channels::Candidate& c);

    // candidatesFor + the duration gate, in one call: the lineup a tuner or a guide uses. `skipped` collects
    // the items dropped for want of a length, so the caller can log them ONCE per build.
    QVector<channels::LineupItem> build(const channels::Channel& ch, QStringList* skipped = nullptr);

    // ---- interstitials (issue #179, increment 2) ---------------------------------------------------------
    // A bumper is an ordinary item with a known length, taken from a folder the user names — the SAME duration
    // gate a programme passes, so a brand-new ident joins the rotation once it has been played through once,
    // exactly as a brand-new episode joins its channel.

    // The video files DIRECTLY inside `dir`, as candidates, sorted by path so every device lays the same
    // break. An EMPTY path yields nothing and is NOT an error (no folder set == no interstitials, which is
    // the default state of the feature). A path that exists but cannot be enumerated — a file, a missing
    // folder, a directory with no read permission — yields nothing AND sets `reason` to a sentence fit to
    // show a person. `reason` is cleared on every call, so a caller may test it without initialising it.
    QVector<channels::Candidate> interstitialCandidates(const QString& dir, QString* reason = nullptr);

    // The channel's bumper pool: its own folder when it names one, otherwise `globalDir`, run through the
    // duration gate. `skipped` collects the files with no known length, for the one log line the caller owes.
    QVector<channels::LineupItem> interstitials(const channels::Channel& ch, const QString& globalDir,
                                                QStringList* skipped = nullptr, QString* reason = nullptr);

    // The seam described in the header: increment 2's sources (and a preset resolver, if the game library
    // ever gains a global enumeration) install themselves here. Called with the channel; returns its
    // candidates. Unset by default, which is what makes an unimplemented source EMPTY rather than wrong.
    void setSourceResolver(channels::SourceKind kind,
                           std::function<QVector<channels::Candidate>(const channels::Channel&)> fn);

    // Reset every installed resolver — the probe's isolation verb; the app never calls it.
    void clearSourceResolvers();
}
