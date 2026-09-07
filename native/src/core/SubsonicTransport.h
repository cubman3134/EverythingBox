// THE SENTENCES A FAILED SUBSONIC REQUEST IS ALLOWED TO PRODUCE (issue #193) — one table, header-only,
// shared by everything in this app that talks to a music server.
//
// It exists because of one specific rule, stated at length in SubsonicClient.h and repeated here because
// this is the file that enforces it: **QNetworkReply::errorString() is a credential**. Subsonic puts `u`,
// `t` and `s` in the QUERY STRING, and Qt's own text for a failed transfer embeds the whole url — so the
// obvious, idiomatic, everybody-writes-it diagnostic line puts the interesting half of the user's password
// into a status bar, a screenshot in a bug report, and a persisted `lastError` in the ini.
//
// The defence is that every transport failure is rendered from the NetworkError ENUM and nothing else. Two
// callers now need that (the browse client and the scrobble provider), and two copies of a sentence table
// would have drifted the first time one of them was corrected — worse, a drift here is not a wording bug, it
// is the door through which a call to errorString() gets added "just in this one place".
#pragma once
#include <QNetworkReply>
#include <QObject>
#include <QString>

namespace SubsonicTransport
{
    inline QString message(QNetworkReply::NetworkError err)
    {
        switch (err)
        {
            case QNetworkReply::HostNotFoundError:
                return QObject::tr("That server could not be found. Check the address.");
            case QNetworkReply::ConnectionRefusedError:
            case QNetworkReply::RemoteHostClosedError:
                return QObject::tr("That server refused the connection. Is it running?");
            case QNetworkReply::TimeoutError:
            case QNetworkReply::OperationCanceledError:
                return QObject::tr("That server took too long to answer.");
            case QNetworkReply::SslHandshakeFailedError:
                return QObject::tr("The secure connection to that server could not be established.");
            case QNetworkReply::AuthenticationRequiredError:
                return QObject::tr("That server refused the sign-in.");
            case QNetworkReply::ContentNotFoundError:
                return QObject::tr("That server answered, but not like a Subsonic server.");
            default:
                return QObject::tr("Could not reach that server.");
        }
    }
}
