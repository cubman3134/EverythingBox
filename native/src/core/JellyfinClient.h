// THE JELLYFIN CLIENT (issue #160, increment 1) — the one object that talks to the configured servers, and
// the TIMEOUT-SAFE FAN-OUT that makes several of them behave like one library.
//
// Everything that could be decided without a socket is in Jellyfin.h (ids, the header, the payload readers,
// the union). What is left here is the part that genuinely needs the world: which servers, which requests,
// how long to wait, and what the user is told when one of them does not answer.
//
// ==================================================================================================
// ONE SHELF, N SERVERS, AND A BUDGET
// ==================================================================================================
// fetchLibrary() issues one /Users/<uid>/Items request per ENABLED server, all at once, each under its own
// deadline. It calls back EXACTLY ONCE, when every leg has either answered or run out of budget — so the
// caller has no partial-state machine to write and no way to be called twice into a half-built shelf.
//
// THE FRIEND'S BOX BEING SWITCHED OFF MUST NOT HOLD UP YOUR OWN SHELF. That is the failure-isolation rule
// #160 asks for, and it is why the deadline is per leg rather than per fan-out: one unreachable server costs
// the budget once, not the sum of the retries a sequential loop would make, and the servers that did answer
// are already in hand when it expires. A leg that misses the budget contributes NOTHING and is reported as
// ONE line (Jellyfin::unavailableNote) — never as an error that empties the shelf.
//
// The budget is a parameter rather than a constant because the two callers want different numbers: a home
// refresh wants to be quick and can afford to leave a slow server out of this pass, while an explicit "open
// this server" wants to wait. Neither number belongs in this file.
//
// ==================================================================================================
// NOTHING BUILDS A MESSAGE OUT OF A REQUEST
// ==================================================================================================
// The rule ListenBrainzClient states for its Authorization header, and Subsonic.h restates for its query
// string. Jellyfin puts the token in a header AND — for the stream url mpv must be handed — in a query
// parameter, so BOTH hazards apply here at once:
//   * QNetworkReply::errorString() embeds the url. There is therefore no call to it in this file; transport
//     failures are rendered from the NetworkError ENUM into fixed sentences of our own.
//   * playUrlFor() returns a string containing the token. It is minted at the moment the player is handed
//     it and is never stored, never logged and never written into a queue, a playlist or a recents row.
// Requests use SameOriginRedirectPolicy for the same reason Subsonic's do: a redirect to another host would
// carry the credential to a server the user never configured.
#pragma once
#include "Jellyfin.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class QNetworkAccessManager;

class JellyfinClient : public QObject
{
    Q_OBJECT
public:
    // One per process. The browse surface and the player both need it, and two of them would be two
    // identities on the wire for one install.
    static JellyfinClient& instance();

    explicit JellyfinClient(QObject* parent = nullptr);

    // ---- The two sign-in steps -------------------------------------------------------------------------
    // Split deliberately, because the FIRST one is what establishes the server's identity and the second is
    // what sends a password. A server whose identity cannot be read is never asked for a password, and is
    // never added.

    // `error` is empty on success. It is one of our own sentences and never contains the url.
    using InfoDone = std::function<void(const Jellyfin::PublicInfo& info, const QString& error)>;
    void fetchPublicInfo(const QString& url, bool allowPlainHttp, int budgetMs, InfoDone done);

    using AuthDone = std::function<void(const Jellyfin::AuthResult& result, const QString& error)>;
    void authenticate(const QString& url, bool allowPlainHttp, const QString& username,
                      const QString& password, int budgetMs, AuthDone done);

    // ---- The merged library ----------------------------------------------------------------------------

    // The union across every ENABLED server, plus one note per server that did not contribute. Called back
    // exactly once. With no enabled servers it calls back immediately with an empty union and no notes —
    // there is nothing to say and nothing to wait for.
    using LibraryDone = std::function<void(const QVector<Jellyfin::UnionItem>& items,
                                           const QStringList& notes)>;
    void fetchLibrary(int budgetMs, LibraryDone done);

    // ---- Playback --------------------------------------------------------------------------------------

    // Resolve a QUALIFIED id to something mpv can open, through the server that owns it.
    //
    // Empty when the id is not qualified, when its server is not configured any more, or when that server is
    // switched off — three different situations with one honest answer, and the caller renders that as
    // "unavailable" rather than erroring at play. #160 asks for exactly that for a removed server's Continue
    // Watching rows.
    //
    // THE RETURN VALUE CARRIES THE TOKEN. Hand it to the player and drop it; never store it.
    QString playUrlFor(const QString& qualifiedId) const;

    // Is this qualified id playable right now — i.e. does it name a configured, enabled server? The question
    // a Continue Watching row asks before it draws itself as available. Costs one ini read; no network.
    bool isAvailable(const QString& qualifiedId) const;

    // ======================================================================================================
    // BROWSE, PLAY AND PROGRESS (issue #83)
    // ======================================================================================================
    // Two shapes, and the difference between them is the whole of the failure policy:
    //
    //   * A FAN-OUT (fetchLibraries, fetchContinueWatching) asks EVERY enabled server and calls back once,
    //     with the union and one note per server that contributed nothing. A server being down costs a note
    //     and nothing else. This is fetchLibrary's shape, and its rule.
    //   * A SINGLE-SERVER FETCH (everything addressed by a qualified id) asks the ONE server that owns the
    //     id and reports an error string when it cannot. There is nothing to isolate here: the user pressed
    //     a row belonging to a particular box, and if that box is down the honest answer is to say so on the
    //     level they opened rather than to draw an empty one.
    //
    // NOTHING BELOW BUILDS A MESSAGE OUT OF A REQUEST, and no error string here has ever seen a url — the
    // rule at the top of this file, which applies with more force now that some of these urls carry a token
    // in their query rather than only in a header.

    // ---- Browse -------------------------------------------------------------------------------------------
    using LibrariesDone = std::function<void(const QVector<Jellyfin::LibraryRef>& libraries,
                                             const QStringList& notes)>;
    void fetchLibraries(int budgetMs, LibrariesDone done);

    // `error` is empty on success. An EMPTY list with an empty error is a real answer: that library has
    // nothing in it, which is not the same as the server refusing to say.
    using ItemsDone = std::function<void(const QVector<Jellyfin::UnionItem>& items, const QString& error)>;
    void fetchLibraryItems(const QString& qualifiedLibraryId, int budgetMs, ItemsDone done);
    void fetchSeasons(const QString& qualifiedSeriesId, int budgetMs, ItemsDone done);
    // seasonId may be empty — /Shows/<series>/Episodes with no season filter is every episode of the show,
    // which is exactly right for a show with no season structure.
    void fetchEpisodes(const QString& qualifiedSeriesId, const QString& qualifiedSeasonId,
                       int budgetMs, ItemsDone done);

    // The server's own Continue Watching, across every enabled server. A fan-out, so one slow box cannot
    // hold up the home screen — which is the surface this feeds, and the one place a stall is least
    // forgivable.
    using ContinueDone = std::function<void(const QVector<Jellyfin::UnionItem>& items,
                                            const QStringList& notes)>;
    void fetchContinueWatching(int budgetMs, ContinueDone done);

    // ---- Play ---------------------------------------------------------------------------------------------
    // EVERYTHING AN OPEN NEEDS, RESOLVED IN ONE CALL, because the three questions are one question: where
    // does this play from, where does it start, and what do the progress reports have to quote?
    struct OpenPlan
    {
        // THE URL CARRIES THE TOKEN. Hand it to the player and drop it; never store it, never log it, and
        // never write it into a row (Jellyfin::recordedPath is what a row records instead).
        QString url;
        QString playSessionId;
        QString mediaSourceId;
        // Where to start, already decided: the server's position when the server answered, else the local
        // mark. Jellyfin::resumeSeconds is the rule and it is applied HERE so that no call site can apply
        // it differently.
        double  resumeSeconds = 0.0;
        bool    serverKnewPosition = false;   // the server answered about this item at all
        bool    played = false;               // ...and says the user has finished it
        bool    transcoding = false;          // the server chose to transcode; display/diagnostic only
        QString error;                        // empty on success; one of our own sentences, never a url
        bool ok() const { return !url.isEmpty(); }
    };
    using OpenDone = std::function<void(const OpenPlan& plan)>;
    void prepareOpen(const QString& qualifiedId, double localResumeSeconds,
                     int audioStreamIndex, int subtitleStreamIndex, int budgetMs, OpenDone done);

    // ---- Progress -----------------------------------------------------------------------------------------
    // FIRE AND FORGET. A progress report that fails changes nothing the user can see, and there is nothing
    // useful to do about it: the next one is ten seconds away. It is emphatically not worth a notice, and
    // the reply is not read, logged or rendered.
    void reportProgress(const QString& qualifiedId, Jellyfin::ProgressEvent ev, double positionSeconds,
                        const QString& playSessionId, const QString& mediaSourceId);

    // ---- Media segments -----------------------------------------------------------------------------------
    // Empty for a server that has no segments for this item, that is too old to have the endpoint at all
    // (pre-10.10 answers 404), or that did not answer. All three are the same thing to the caller — one
    // fewer provider tier — which is why this has no error channel.
    using SegmentsDone = std::function<void(const QVector<Jellyfin::RemoteSegment>& segments)>;
    void fetchMediaSegments(const QString& qualifiedId, int budgetMs, SegmentsDone done);

private:
    // The shared tail of the three single-server list fetches: they differ only in the path and the query,
    // and three copies of the reply handling is three chances to disagree about what an empty answer means.
    void fetchItemList(const QString& qualifiedId, const QString& path, const QString& query,
                       int budgetMs, ItemsDone done);
    QNetworkAccessManager* nam();
    QNetworkAccessManager* nam_ = nullptr;
};
