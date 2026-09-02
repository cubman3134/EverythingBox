#include "LiveTvResolver.h"

#include "StreamResolver.h"
#include "../core/AppBrand.h"
#include "../core/IptvSourceStore.h"
#include "../core/LiveTvMigrate.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

LiveTvResolver::LiveTvResolver(QNetworkAccessManager* nam, QObject* parent)
    : QObject(parent), nam_(nam)
{
}

void LiveTvResolver::resolve(const QString& channelId)
{
    ++gen_;                       // supersede anything in flight
    id_ = channelId;
    at_ = 0;
    sourceUrls_.clear();
    for (const IptvSource& s : IptvSourceStore::list())
        if (!s.url.isEmpty()) sourceUrls_ << s.url;   // the user's own order IS the search order
    step();
}

void LiveTvResolver::step()
{
    if (id_.isEmpty() || at_ >= sourceUrls_.size()) { emit unavailable(id_); return; }
    const QString src = sourceUrls_.at(at_);
    const int gen = gen_;

    if (!src.contains(QStringLiteral("://")))
    {
        // A local .m3u/.m3u8 file: read it directly, no network — the shelf's own rule.
        QFile f(src);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) deliver(QString::fromUtf8(f.readAll()), true);
        else                                               deliver(QString(), false);
        return;
    }

    QNetworkRequest req{ QUrl(src) };
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen] {
        reply->deleteLater();
        if (gen != gen_) return;   // a later resolve() superseded this one
        if (reply->error() != QNetworkReply::NoError) { deliver(QString(), false); return; }
        deliver(QString::fromUtf8(reply->readAll()), true);
    });
}

void LiveTvResolver::deliver(const QString& text, bool ok)
{
    const QString src = at_ < sourceUrls_.size() ? sourceUrls_.at(at_) : QString();
    ++at_;                                      // whatever happens, this source has had its turn
    if (!ok || text.isEmpty()) { step(); return; }   // a dead source is not an answer; try the next one

    const QVector<M3uEntry> entries = StreamResolver::parseM3u(text, src);
    if (entries.isEmpty()) { step(); return; }

    QVector<LiveTvIdentity::Channel> channels;
    channels.reserve(entries.size());
    for (const M3uEntry& e : entries) channels.push_back({ e.tvgId, e.tvgName, e.title, e.url });

    // The list is here and parsed, so the legacy rows it can name get named — see the header.
    LiveTvMigrate::withChannels(channels);

    const QString url = LiveTvIdentity::urlFor(channels, id_);
    if (url.isEmpty()) { step(); return; }
    emit resolved(id_, url);
}
