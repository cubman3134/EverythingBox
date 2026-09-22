#include "ThemeShots.h"

#include "AppBrand.h"
#include "CoverFetch.h"
#include "MetaCache.h"
#include "ThemeRegistry.h"

#include <QCryptographicHash>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QVector>

#include <memory>

namespace {

// The callbacks owed for a (key|role) whose request is in flight. A hash of LISTS rather than of single
// callbacks because both surfaces can ask for the same picture twice in one pass — the themed panel is
// rebuilt while a fetch from the previous build is still running, and two registries may serve the same
// screenshot url — and the second asker must not start a second request nor be left without an answer.
QHash<QString, QVector<std::function<void(const QString&)>>>& pending()
{
    static QHash<QString, QVector<std::function<void(const QString&)>>> p;
    return p;
}

// The object every reply's destroyed() connection is made THROUGH, and never the reply itself.
//
// A lambda whose RECEIVER CONTEXT is the object emitting destroyed() is delivered while that object is
// already inside ~QObject with its connection list coming apart underneath the delivery. It segfaults, and
// it segfaults at the least helpful moment: probe_themereg block 21 reproduced it inside
// ~QNetworkAccessManager, when the manager took its still-unfinished replies with it. Routing the
// connection through a neutral object that outlives them makes the delivery ordinary.
QObject* destroyGuard()
{
    static QObject g;
    return &g;
}

void settle(const QString& tag, const QString& path)
{
    const QVector<std::function<void(const QString&)>> waiters = pending().take(tag);
    for (const std::function<void(const QString&)>& fn : waiters)
        if (fn) fn(path);
}

} // namespace

QString ThemeShots::cacheKey(const QString& folder)
{
    if (folder.isEmpty()) return QString();
    return QStringLiteral("themeshot:") + folder;
}

QString ThemeShots::cacheRole(const QString& url)
{
    if (url.isEmpty()) return QString();
    // The WHOLE url is hashed, not its file name: two registries serving "shot.png" from different
    // directories are two pictures, and a registry that repoints the same screenshot at a new file must get
    // a new role or the old bytes are "already cached" for ever. Md5 for the same reason MetaCache's
    // fixedImageRole uses it — this is a cache file name, not a security claim — and truncated to 16 hex
    // characters so the name stays short on a filesystem that will also hold the theme.
    return QStringLiteral("shot-")
           + QString::fromLatin1(
               QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex().left(16));
}

QString ThemeShots::cachedPath(const QString& folder, const QString& url)
{
    const QString key = cacheKey(folder), role = cacheRole(url);
    if (key.isEmpty() || role.isEmpty()) return QString();
    // verifiedImagePath, not imagePath: a file whose bytes are not a picture is treated as absent (and
    // removed), so a page stored by an older build is re-fetched rather than drawn as a broken image.
    return MetaCache::verifiedImagePath(key, role);
}

bool ThemeShots::acceptBytes(const QByteArray& body)
{
    if (body.isEmpty()) return false;
    if (qint64(body.size()) > ThemeRegistry::kMaxScreenshotBytes) return false;
    return CoverFetch::isPicture(body);
}

void ThemeShots::fetch(QNetworkAccessManager* nam, const QString& folder, const QString& url,
                       std::function<void(const QString&)> done)
{
    const QString key = cacheKey(folder), role = cacheRole(url);
    if (!nam || key.isEmpty() || role.isEmpty() || url.isEmpty())
    { if (done) done(QString()); return; }

    // Already on disk: answer now, with no request at all. This is the whole of the caching behaviour from
    // the caller's side — a row that has been drawn before draws its thumbnail in the same pass it is built.
    const QString have = cachedPath(folder, url);
    if (!have.isEmpty()) { if (done) done(have); return; }

    const QString tag = key + QLatin1Char('|') + role;
    const bool alreadyInFlight = pending().contains(tag);
    pending()[tag].append(done);
    if (alreadyInFlight) return;

    QNetworkRequest req{ QUrl(url) };
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    // NoLessSafeRedirect: a redirect may not take an https screenshot down to http. It CAN still take it to
    // another host, which is why the bytes are judged on arrival rather than the url being trusted once it
    // passed the host rule.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // An inactivity bound, so a hung server cannot hold this tag — and with it every later ask for the same
    // picture — for the rest of the session. Same constant the cover fetches use.
    req.setTransferTimeout(CoverFetch::kTransferTimeoutMs);
    QNetworkReply* reply = nam->get(req);

    // THE CAP, APPLIED AS THE BYTES ARRIVE, in the two places the two kinds of oversize response are
    // catchable — neither covers the other.
    //
    // First a bound on the RESIDENT bytes: +1 so the byte that crosses the budget is still seen rather than
    // the transfer stalling one byte short of being refusable.
    reply->setReadBufferSize(ThemeRegistry::kMaxScreenshotBytes + 1);

    // THE BODY IS DRAINED AS IT ARRIVES rather than read once at finished(), and that is not a style
    // choice. With a read-buffer limit and nothing consuming it, Qt's HTTP backend fills the buffer, PAUSES
    // the transfer, and then nothing moves at all — no further progress, no finished — until the inactivity
    // timeout fires twenty seconds later. Measured on loopback in probe_themereg block 21: the refusal was
    // correct and arrived twenty seconds late, which across a gallery is a handful of sockets held open for
    // nothing. Draining keeps the transfer live, so the moment the accumulated body crosses the budget we
    // can abort it — which is what "enforced while it is read" has to mean.
    auto body = std::make_shared<QByteArray>();
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, body] {
        body->append(reply->readAll());
        if (qint64(body->size()) <= ThemeRegistry::kMaxScreenshotBytes) return;
        body->clear();          // refused: there is nothing worth keeping, and it is the larger half
        reply->abort();
    });

    // …and the DECLARED length, which is the other kind: a response that announces itself over budget is
    // dropped before its body arrives rather than after. `total` is -1 when the server declares nothing,
    // which passes — that case is the drain above.
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                     [reply, body](qint64 received, qint64 total) {
        if (received <= ThemeRegistry::kMaxScreenshotBytes && total <= ThemeRegistry::kMaxScreenshotBytes)
            return;
        body->clear();
        reply->abort();
    });

    // A reply destroyed WITHOUT finishing would otherwise leave this tag in `pending` for ever, and every
    // later ask for that picture would attach to a request that no longer exists and never hear back. It is
    // not hypothetical: the classic browser owns its own manager, so closing the gallery while a thumbnail
    // is in flight destroys the reply, and the next visit builds a new dialog and a new manager around a tag
    // the old one stranded. settle() on a tag that was already answered is a no-op (take() of an absent
    // key), so this is harmless after the ordinary path too. Note the CONTEXT — see destroyGuard.
    QObject::connect(reply, &QObject::destroyed, destroyGuard(), [tag] { settle(tag, QString()); });

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, key, role, tag, body] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) { settle(tag, QString()); return; }
        body->append(reply->readAll());    // whatever the last turn left unread
        // Belt to the arrival-time brace (an over-budget body that finished in the same turn as the abort)
        // AND the picture rule. Refused bytes are not stored: the role stays empty, so the next time this
        // row is built the picture is asked for again rather than served from a cached error page.
        if (!acceptBytes(*body)) { settle(tag, QString()); return; }
        MetaCache::storeImage(key, role, reply->url().toString(),
                              reply->header(QNetworkRequest::ContentTypeHeader).toString(), *body);
        settle(tag, MetaCache::verifiedImagePath(key, role));
    });
}
