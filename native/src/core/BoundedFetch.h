// One blocking GET that refuses to become a large download. It reads the response's own declaration of its
// size and stops as soon as that declaration — or the bytes themselves — cross a ceiling the CALLER sets.
//
// It exists because "fetch this small file" and "download this large one" are different operations with
// different UI, and the only thing that can tell them apart is the response. A file's NAME cannot: a romhack
// patch's format is asserted by its source and never sniffed, and a two-byte tweak and a disc-scale rebuild
// arrive under the same extension. A HEAD would answer the question at the cost of a round trip on every
// call, for the common case that needs none, and only where the route answers HEAD at all.
//
// So the request judges itself. Under the ceiling this is exactly a blocking fetch and the body is returned.
// Over it, the transfer is ABANDONED and the caller is told so — the point being that the caller can then
// hand the same url to something built for large transfers, having spent one response head rather than the
// whole file.
//
// Holds no policy: the ceiling and the deadline are parameters, and nothing here logs. What counts as "too
// big", what to say about it, and where a refused url goes next all belong to the call site.
#pragma once
#include <QByteArray>
#include <QString>

namespace BoundedFetch
{
    struct Result
    {
        enum Verdict
        {
            Ok,      // the whole body arrived and fitted; `body` is it
            TooBig,  // the response was over the ceiling and was abandoned; `body` is empty
            Failed,  // no response, a refused one, or the deadline; `body` is empty
        };

        Verdict    verdict  = Failed;
        QByteArray body;          // the complete body, and ONLY when verdict == Ok
        // What the RESPONSE said about its own length, or -1 when it declared none. Reported rather than
        // inferred: "the server said 500 MB" and "the server said nothing and we stopped at the ceiling" are
        // different facts, and only the first can be put in a sentence.
        qint64     declared = -1;
        // Bytes actually received before the verdict was reached, whatever the verdict. This is the property
        // that makes the ceiling testable: a TooBig returned after quietly reading the whole body is the
        // exact bug this unit exists to prevent, and it is indistinguishable from a correct one without this.
        qint64     read     = 0;
        int        status   = 0;  // HTTP status, or 0 when no response head ever arrived
        QString    error;         // Qt's reason, for a log — never for a user; one of its values is
                                  // "Operation canceled", which is the one thing this must not be read as
    };

    // Fetch `url`, giving up after `timeoutMs`, refusing anything over `ceilingBytes`.
    //
    // Redirects are followed (NoLessSafeRedirectPolicy). That is a decision and not a default left in place:
    // the request carries no headers, no cookies and no credentials, so a hop leaks nothing, and a server
    // behind a reverse proxy or a CDN needs the hop followed or its files are simply unreachable.
    Result get(const QString& url, int timeoutMs, qint64 ceilingBytes);
}
