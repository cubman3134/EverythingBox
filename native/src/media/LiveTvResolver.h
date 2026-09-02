// TURNING A LIVE TV IDENTITY BACK INTO A PLAYABLE URL (issue #203).
//
// A favourite or playlist entry now names a CHANNEL (`livetv:<tvg-id>`, `livetv:name:<name>`) rather than a
// stream. Opening one therefore has to ask "where is this channel, right now, on the sources THIS device
// has" — which is a fetch, because a source's channels are deliberately never persisted (#75: a stale channel
// list across sessions is the thing that feature exists to avoid). That is what this does.
//
// SEQUENTIAL, IN SOURCE ORDER, STOPPING AT THE FIRST MATCH. The user's source list is a preference order, and
// a channel carried by two providers should come from the one they put first. Sequential also means the
// common case — the channel is in the first source — costs exactly one request, and a device with several
// sources only pays for the ones it has to look through.
//
// EVERY LIST IT FETCHES IS ALSO A MIGRATION OPPORTUNITY: LiveTvMigrate::withChannels runs on each parsed
// source, so simply opening a favourite repairs whatever legacy rows that source can name. See LiveTvMigrate.h
// for why the repair is driven by data arriving rather than by a startup pass.
//
// A RESOLVE THAT FINDS NOTHING IS NOT A FAILURE TO REPORT AS AN ERROR AND NOT A REASON TO DELETE ANYTHING. It
// means this device does not currently carry that channel — the source was removed, the provider dropped it,
// or the row came from a peer that has a source this one does not. `unavailable` says so; the row stays.
#pragma once
#include "../core/LiveTvIdentity.h"
#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;

class LiveTvResolver : public QObject
{
    Q_OBJECT
public:
    // `nam` is borrowed, never owned — the app's shared manager, the same one the Live TV shelf fetches with.
    explicit LiveTvResolver(QNetworkAccessManager* nam, QObject* parent = nullptr);

    // Look `channelId` up across this profile's sources. Exactly one signal is emitted, always asynchronously
    // with respect to nothing in particular: a local .m3u file resolves without a round trip, so callers must
    // not assume a return-then-signal ordering. A second call supersedes the first (its replies are dropped).
    void resolve(const QString& channelId);

signals:
    void resolved(const QString& channelId, const QString& url);
    void unavailable(const QString& channelId);

private:
    void step();                                  // try source `at_`, or give up
    void deliver(const QString& text, bool ok);   // one source's body -> match, or move on

    QNetworkAccessManager* nam_ = nullptr;
    QString id_;
    QStringList sourceUrls_;
    int at_ = 0;
    int gen_ = 0;
};
