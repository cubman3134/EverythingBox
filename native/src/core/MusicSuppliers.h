// WHICH MUSIC SUPPLIERS EXIST, ASKED ONCE (issue #384).
//
// There are four suppliers of music: the local folder (#74), every Subsonic server (#193), every ENABLED
// Jellyfin server and every EverythingBox-server music shelf (#194 increment 3). Two questions are asked of
// them, and they used to be answered in two places with two different lists:
//
//   * is there a Music tab at all?           the tab gate named the local folder and Subsonic, and nothing else
//   * is there more than one to merge?       musicMergePossible() counted all four
//
// So a user whose whole library is a Jellyfin box, or a shelf on their own server, had music the merge knew
// about and no door to reach it. The fix is not a second, longer list at the tab: it is ONE count that both
// questions read, with two thresholds - a tab needs a supplier, a merge needs two. Written as one definition
// they cannot disagree about which sources exist, which is the whole of the bug.
//
// ==================================================================================================
// EVERY TERM IS A SETTINGS READ OR AN IN-MEMORY LIST - NEVER THE NETWORK
// ==================================================================================================
// The tab gate runs on every home refresh. A term that reached a server to decide whether to draw a tab would
// make the home screen wait on a box that may be switched off. So this file takes plain values, and the
// caller (HomeView::musicSuppliers) fills them from MusicLibrary::hasLibrary, the two server stores and
// ServerMusicClient's pushed-in shelf list - none of which touches a socket.
//
// ==================================================================================================
// JELLYFIN COUNTS ENABLED SERVERS; A METADATA ADD-ON'S `music` CATALOGUE COUNTS FOR NOTHING
// ==================================================================================================
// `enabled` is the switch that means "get this library out of the way for the evening". A switched-off
// server is a supplier that is not supplying, so it neither opens the tab nor puts an install into the merged
// path. The filter is applied HERE, over the whole server list, so neither caller can forget it.
//
// A shelf qualifies only when it is served by a REMOTE server the user connected over our own add-on
// protocol. A bundled metadata add-on has a catalogue of type `music` too (the AIO catalog's MusicBrainz
// shelf); a database of every record ever pressed is not somebody's library, and letting it open a Music tab
// would give every install one - backed by nothing the user owns. That rule is the two shelf predicates at
// the bottom, which HomeView::refreshMusicShelves applies and probe_musicsources pins.
//
// Header-only and QtCore-only on purpose: probe_musicsources drives it with no store, no add-on runtime and
// no HomeView.
#pragma once
#include "JellyfinServerStore.h"

#include <QList>
#include <QString>

namespace MusicSuppliers
{
    struct Suppliers
    {
        bool                  localLibrary    = false;   // MusicLibrary::hasLibrary(): a root that EXISTS
        int                   subsonicServers = 0;       // SubsonicServerStore::list().size() - no on/off switch
        QList<JellyfinServer> jellyfin;                  // JellyfinServerStore::list(), ALL of them
        int                   serverShelves   = 0;       // ServerMusicClient::shelves().size()
    };

    inline int enabledJellyfin(const Suppliers& s)
    {
        int n = 0;
        for (const JellyfinServer& j : s.jellyfin) if (j.enabled) ++n;
        return n;
    }

    // THE ONE COUNT. A count of suppliers, not of content: the local folder counts as soon as its root exists,
    // because a scan is asynchronous and "no tracks yet" and "no library" want opposite answers.
    inline int count(const Suppliers& s)
    {
        return (s.localLibrary ? 1 : 0) + s.subsonicServers + enabledJellyfin(s) + s.serverShelves;
    }

    // ...and its two thresholds.
    inline bool tabOffered(const Suppliers& s)    { return count(s) >= 1; }
    inline bool mergePossible(const Suppliers& s) { return count(s) >= 2; }

    // THE MUSIC ROOT'S ARTIST LIST, for an install that has exactly one supplier and it is Jellyfin or a shelf.
    //
    // A single-supplier install renders the root from the LOCAL index plus the Subsonic "Music Servers" door,
    // and nothing else. That is exactly right for the two suppliers it was written for, and it is the same
    // two-source rule the tab had: opened by a Jellyfin-only user it is an empty page. So the root also takes
    // the supplier-list path (ask each remote supplier for its artists, show what has arrived) whenever a
    // Jellyfin or shelf supplier exists. With one live source MusicMerge::merge returns that source's index
    // verbatim, so nothing is merged that is not there. A local-only or Subsonic-only install answers false
    // and keeps the root it has always had.
    inline bool rootListsRemote(const Suppliers& s)
    {
        return mergePossible(s) || enabledJellyfin(s) + s.serverShelves > 0;
    }

    // WHAT THE ROOT SAYS WHEN IT HAS NO ARTISTS. Only asked after the local index was found empty.
    //   None     say nothing: a Subsonic server is configured and the "Music Servers" door is on the page (#193)
    //   Local    today's local-folder sentences ("no folder yet" / "scanning" / "nothing in it")
    //   Loading  the only suppliers are remote ones and one of them has not answered yet
    //   Refused  ...every one has answered and at least one refused
    //   Nothing  ...they answered and hold no music
    // A local folder keeps its own sentences whatever else is configured, so a local user reads exactly what
    // they read before. Telling a Jellyfin-only user to "choose a music folder" is the case this replaces.
    enum class EmptyNote { None, Local, Loading, Refused, Nothing };
    inline EmptyNote emptyNote(const Suppliers& s, int remoteInFlight, bool anyRefused)
    {
        if (s.subsonicServers > 0) return EmptyNote::None;
        if (s.localLibrary || enabledJellyfin(s) + s.serverShelves == 0) return EmptyNote::Local;
        if (remoteInFlight > 0) return EmptyNote::Loading;
        if (anyRefused) return EmptyNote::Refused;
        return EmptyNote::Nothing;
    }

    // ---- THE SHELF RULE (#194 increment 3), as data -------------------------------------------------------
    // A SOURCE may serve a music shelf only when it is a remote server over our own protocol (not a bundled
    // local add-on, not a third-party Stremio add-on) and the user has it switched on.
    inline bool sourceMayServeShelf(bool remoteHttp, bool stremio, bool enabled)
    {
        return remoteHttp && !stremio && enabled;
    }
    // ...and its CATALOGUE is the shelf when it is of type `music` and can be browsed at all.
    inline bool catalogIsShelf(const QString& type, bool searchOnly, bool hasSkipReason)
    {
        return type == QLatin1String("music") && !searchOnly && !hasSkipReason;
    }
}
