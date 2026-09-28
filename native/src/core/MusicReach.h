// WHICH MUSIC SUPPLIERS ARE ANSWERING RIGHT NOW (issue #194, offline fallback) — the reachability input to
// MusicId::pickAutoSource's play-time overload.
//
// ==================================================================================================
// WHAT THE SUPPLIERS ALREADY KNOW, AND NOTHING ELSE
// ==================================================================================================
// There is no pinging here, periodic or otherwise. Every music supplier already talks to its server through
// one request helper (SubsonicClient::request, JellyfinMusicClient::request, ServerMusicClient::request, and
// each client's cover fetch), and each of those already finds out, reply by reply, whether the box answered.
// They file that answer here as it lands:
//
//   * a request that failed AT THE TRANSPORT — refused, no such host, timed out, cancelled by the client's own
//     budget, TLS refused, or a gateway's 5xx in front of a server that is down — marks the supplier
//     UNREACHABLE;
//   * any reply that came back from the server — a success, a Subsonic failure envelope, a 401, a 404 —
//     marks it REACHABLE again. The box is up; whatever else is wrong with the request is not "offline".
//
// So a supplier is unreachable after a failed request until a later one succeeds, which is the rule the
// issue asks for. A supplier nobody has asked yet is reachable: absence of evidence is not a verdict, and a
// fresh start with every server marked down would make the first play of the session a fallback for no reason.
//
// The one input that is not a request is THE NETWORK BEING DOWN AT ALL, which the app learns from Qt's
// QNetworkInformation and hands in through setOffline(). While it holds, every remote supplier is unreachable
// and a local copy — always reachable, by MusicId::Reachability's own rule — is what plays.
//
// ==================================================================================================
// HEADER-ONLY AND QtCore-ONLY
// ==================================================================================================
// The three client .cpp files that report into this are linked into the app and into several probes, and
// none of those targets has to learn a new translation unit for it. The state is one function-local static:
// every caller runs on the GUI thread (network replies are delivered there), so there is nothing to lock.
// The error code is an int rather than QNetworkReply::NetworkError so that this header does not pull in
// QtNetwork; the ranges below are Qt's own documented numbering of that enum.
#pragma once
#include "MusicId.h"

#include <QSet>
#include <QString>

namespace MusicReach
{
    // THE ONE CLASSIFIER. A QNetworkReply::NetworkError, as an int, answered "did the box fail to answer?".
    //   1..99    the network layer: refused, closed, not found, timeout, cancelled (a client's own budget
    //            aborts the reply), TLS handshake, temporary failure, unknown network error
    //   101..199 the proxy the request went through
    //   401..499 the SERVER side: 500, 501, 503 and every other 5xx. A reverse proxy in front of a music
    //            server that is switched off answers 502/503, and that is the box being down.
    // Everything else — NoError, a 4xx (201..299), a protocol error (301..399) — means something answered.
    inline bool isUnreachableError(int qnetworkReplyError)
    {
        const int e = qnetworkReplyError;
        return (e >= 1 && e <= 199) || (e >= 401 && e <= 499);
    }

    struct State
    {
        QSet<QString> down;
        bool          offline = false;
        quint64       generation = 0;   // bumped on every change, so a surface can tell it is stale
    };
    inline State& state() { static State s; return s; }

    inline void noteReached(const QString& supplierId)
    {
        if (supplierId.isEmpty()) return;
        if (state().down.remove(supplierId)) ++state().generation;
    }
    inline void noteUnreachable(const QString& supplierId)
    {
        if (supplierId.isEmpty() || state().down.contains(supplierId)) return;
        state().down.insert(supplierId);
        ++state().generation;
    }
    // What every client's reply handler calls: the reply's error code, classified above.
    inline void noteAnswer(const QString& supplierId, int qnetworkReplyError)
    {
        if (isUnreachableError(qnetworkReplyError)) noteUnreachable(supplierId);
        else                                        noteReached(supplierId);
    }

    inline void setOffline(bool offline)
    {
        if (state().offline == offline) return;
        state().offline = offline;
        ++state().generation;
    }

    inline MusicId::Reachability current()
    {
        MusicId::Reachability r;
        r.unreachable = state().down;
        r.offline     = state().offline;
        return r;
    }
    inline quint64 generation() { return state().generation; }

    // For probes, which drive several suppliers through one process.
    inline void reset() { state() = State{}; }
}
