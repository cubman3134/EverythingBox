// WHAT A COVER REQUEST ANSWERED (issue #370) — the one rule every music supplier's cover prefetch follows, so
// that three copies of it cannot drift apart.
//
// A prefetch's `then` means "new artwork landed, re-render", and a browse level wires it to a re-render that
// re-runs the prefetch over the same rows (HomeView::scheduleMusicArtRefresh). So two questions have to be
// answered after every reply, and neither may be answered by the reply having ARRIVED:
//
//   Did anything land?          Only what is on disk afterwards says so. `then` fires for that and nothing
//                               else: firing it for an answer that stored nothing re-renders the level, which
//                               finds nothing on disk and nothing in flight, and asks again — for ever. That
//                               loop is #370, and before it the Jellyfin and server-shelf clients fired `then`
//                               even for a cover that was ALREADY cached.
//
//   Should it be asked again?   That depends on who said no.
//
//     Image    a body that may be a picture. Stored; `then` fires if, and only if, it is then on disk.
//     Absent   the SERVER answered, and the answer was "no picture": a successful empty body, a 404 or a 410
//              (or, for Subsonic, the protocol's own "not found" — see Subsonic::coverAnswer). Remembered as
//              "no art" for the SESSION, in memory only and keyed on the album key: never persisted, so art the
//              server gains later still arrives in a later session; and never a url, which can carry a
//              credential.
//     Retry    nothing was answered: a refused connection, a TIMEOUT, a 5xx, a refused credential. Not
//              remembered, because it may work next time — and not a reason to re-render either, so it is asked
//              again only when the level re-renders for some other reason.
#pragma once
#include <QByteArray>

namespace CoverFetch
{
    enum class Answer { Image, Absent, Retry };

    // `transportOk` is QNetworkReply::NoError. `httpStatus` is the status line's code, and 0 when there was
    // none — a refused connection, and a timeout, both look like that. The body is only read when the transfer
    // succeeded: a failed reply's body is an error page, not a picture.
    inline Answer classify(bool transportOk, int httpStatus, const QByteArray& body)
    {
        if (!transportOk)
            return (httpStatus == 404 || httpStatus == 410) ? Answer::Absent : Answer::Retry;
        return body.isEmpty() ? Answer::Absent : Answer::Image;
    }

    // THE TIMEOUT RULE. A cover request that goes this long without a byte moving is given up (it is
    // QNetworkRequest's transfer timeout, an INACTIVITY bound rather than a total), and given up is Retry, never
    // Absent: a slow server has not said "no art". It is bounded at all because without it a hung request
    // holds its in-flight tag — and with it every later ask for that cover — for the rest of the session.
    constexpr int kTransferTimeoutMs = 20000;
}
