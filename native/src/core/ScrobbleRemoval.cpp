#include "ScrobbleRemoval.h"

#include <QCoreApplication>

namespace {

// One context for every string here, so a translator sees the three moments together and can keep them
// consistent with one another. QCoreApplication::translate rather than a QObject's tr() because nothing in
// this file is an object: these are pure functions, which is exactly what lets a headless probe assert them.
const char* kCtx = "ScrobbleRemoval";

} // namespace

QString ScrobbleRemoval::offerMessage(const QString& serverName, int pending)
{
    // TWO PARAGRAPHS AND BOTH ARE LOAD-BEARING. The first is the removal confirmation this feature has
    // always had — nothing on the server itself changes, which is the thing people actually fear about a
    // Remove button. The second is #337: what is about to happen to listening history the user generated,
    // said BEFORE it happens, with the number in it.
    return QCoreApplication::translate(kCtx,
               "Remove “%1” and forget its sign-in? Nothing on the server itself is changed.")
               .arg(serverName)
         + QStringLiteral("\n\n")
         + QCoreApplication::translate(kCtx,
               "%n play(s) from this server have not been reported to it yet. This is the last moment they "
               "can be sent — once the sign-in is forgotten there is nothing left to send them to, so "
               "anything not sent now is deleted along with the server.",
               "unsent listens at removal time", pending);
}

QString ScrobbleRemoval::sendLabel(int pending)
{
    return QCoreApplication::translate(kCtx, "Send %n play(s), then remove", "", pending);
}

QString ScrobbleRemoval::discardLabel(int pending)
{
    return QCoreApplication::translate(kCtx, "Remove and discard %n play(s)", "", pending);
}

QString ScrobbleRemoval::outcomeMessage(const QString& serverName, int sent, int discarded,
                                        const QString& why)
{
    // THE THREE ENDINGS. A flush that half worked is the one a careless report gets wrong, in whichever
    // direction it rounds: "sent" loses the listens silently all over again, and "failed" makes somebody
    // throw away a server they had no reason to keep. So the middle arm states BOTH numbers.
    QString s;
    if (discarded <= 0 && sent > 0)
        s = QCoreApplication::translate(kCtx, "All %n play(s) were sent to “%1” before it was removed.",
                                        "", sent).arg(serverName);
    else if (discarded <= 0)
        s = QCoreApplication::translate(kCtx, "“%1” was removed.").arg(serverName);
    else if (sent > 0)
        s = QCoreApplication::translate(kCtx, "%n play(s) were sent to “%1”.", "", sent).arg(serverName)
          + QLatin1Char(' ')
          + QCoreApplication::translate(kCtx,
                "The remaining %n could not be sent, and have been discarded with the server.", "",
                discarded);
    else
        s = QCoreApplication::translate(kCtx,
                "None of the %n play(s) waiting for “%1” could be sent. They have been discarded with the "
                "server.", "", discarded).arg(serverName);

    // The service's own words, LAST and only when something is actually owed an explanation. A reason
    // printed beside "all 6 were sent" would be describing a failure that did not happen.
    if (discarded > 0 && !why.isEmpty()) s += QLatin1Char(' ') + why;
    return s;
}

QString ScrobbleRemoval::sweepMessage(int queues, int plays)
{
    // NOTHING CAN BE OFFERED HERE, and the message says why rather than leaving somebody to wonder where the
    // "send them" button is: the sign-in went with the server, so there is no longer anything to send them
    // to. That difference is the whole reason the offer above has to exist at the one moment it does.
    const QString found = queues > 1
        ? QCoreApplication::translate(kCtx,
              "%n play(s) are still waiting to be reported to music servers that are no longer set up.",
              "orphaned scrobble queues", plays)
        : QCoreApplication::translate(kCtx,
              "%n play(s) are still waiting to be reported to a music server that is no longer set up.",
              "orphaned scrobble queue", plays);
    return found + QLatin1Char(' ')
         + (queues > 1
                ? QCoreApplication::translate(kCtx,
                      "Their sign-ins were forgotten when those servers were removed, so nothing can send "
                      "them now. They can be deleted, or left where they are.")
                : QCoreApplication::translate(kCtx,
                      "Its sign-in was forgotten when that server was removed, so nothing can send them "
                      "now. They can be deleted, or left where they are."));
}

QString ScrobbleRemoval::sweepKeepLabel()
{
    return QCoreApplication::translate(kCtx, "Keep them for now");
}

QString ScrobbleRemoval::sweepDiscardLabel(int plays)
{
    return QCoreApplication::translate(kCtx, "Delete %n play(s)", "", plays);
}
