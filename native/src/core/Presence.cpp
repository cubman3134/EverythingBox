#include "Presence.h"
#include "AppBrand.h"

#include <QByteArray>
#include <QtGlobal>

namespace {

// A UTF-8 continuation byte is 10xxxxxx. Cutting on one leaves an orphaned fragment that makes Discord
// discard the whole payload, which presents as "presence stopped updating" and nothing else.
inline bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

} // namespace

bool Presence::Activity::operator==(const Activity& o) const
{
    return valid      == o.valid      && type       == o.type
        && details    == o.details    && state      == o.state
        && largeImage == o.largeImage && largeText  == o.largeText
        && smallImage == o.smallImage && smallText  == o.smallText
        && startUnix  == o.startUnix  && endUnix    == o.endUnix
        && buttons    == o.buttons;
}

int Presence::typeFor(Kind k)
{
    switch (k) {
    case Kind::Movie: case Kind::Episode: case Kind::LiveTv:  return kWatching;
    case Kind::Music: case Kind::Audiobook:                   return kListening;
    // Reading takes Playing because Discord has no fourth verb — the header reads "Playing EverythingBox"
    // and the body carries "Reading". Wrong in the header, honest in the two lines a human reads.
    case Kind::Game: case Kind::PcGame: case Kind::Reading:   return kPlaying;
    case Kind::None:                                          break;
    }
    return kPlaying;
}

QString Presence::fallbackAsset(Kind k)
{
    switch (k) {
    case Kind::Movie:     return QStringLiteral("movie");
    case Kind::Episode:   return QStringLiteral("tv");
    case Kind::LiveTv:    return QStringLiteral("livetv");
    case Kind::Music:     return QStringLiteral("music");
    case Kind::Audiobook: return QStringLiteral("audiobook");
    case Kind::Game: case Kind::PcGame: return QStringLiteral("game");
    case Kind::Reading:   return QStringLiteral("book");
    case Kind::None:      break;
    }
    return QStringLiteral("logo");
}

QString Presence::kindLabel(Kind k)
{
    switch (k) {
    case Kind::Movie:     return QStringLiteral("Movie");
    case Kind::Episode:   return QStringLiteral("TV");
    case Kind::LiveTv:    return QStringLiteral("Live TV");
    case Kind::Music:     return QStringLiteral("Music");
    case Kind::Audiobook: return QStringLiteral("Audiobook");
    case Kind::Game:      return QStringLiteral("Game");
    case Kind::PcGame:    return QStringLiteral("Game");
    case Kind::Reading:   return QStringLiteral("Reading");
    case Kind::None:      break;
    }
    return QString::fromLatin1(AppBrand::kName);
}

bool Presence::hasCountdown(Kind k)
{
    switch (k) {
    case Kind::Movie: case Kind::Episode: case Kind::Music: case Kind::Audiobook: return true;
    default: return false;
    }
}

QString Presence::clampUtf8(const QString& s, int maxBytes)
{
    if (maxBytes <= 0) return QString();
    const QByteArray u = s.toUtf8();
    if (u.size() <= maxBytes) return s;
    int cut = maxBytes;
    while (cut > 0 && isContinuation(u.at(cut))) --cut;
    return QString::fromUtf8(u.left(cut));
}

Presence::Activity Presence::build(const Item& item, double positionSec, double durationSec,
                                   bool paused, qint64 nowUnix)
{
    Activity a;
    if (item.kind == Kind::None || item.title.isEmpty()) return a;   // valid stays false: send nothing

    a.valid      = true;
    a.type       = typeFor(item.kind);
    a.details    = clampUtf8(item.title, kMaxFieldBytes);
    a.state      = paused ? QStringLiteral("Paused") : clampUtf8(item.subtitle, kMaxFieldBytes);
    a.largeImage = item.artUrl.startsWith(QLatin1String("https://")) ? item.artUrl
                                                                     : fallbackAsset(item.kind);
    a.largeText  = clampUtf8(item.title, kMaxFieldBytes);
    a.smallImage = fallbackAsset(item.kind);
    a.smallText  = kindLabel(item.kind);

    // A paused card carries NO timestamp at all. Discord runs the countdown client-side from `end`, so an end
    // left set through a pause counts a stopped film down to zero and then sits there lying.
    if (!paused) {
        const double pos = qMax(0.0, positionSec);
        if (hasCountdown(item.kind) && durationSec > 0.0 && pos < durationSec)
            a.endUnix = nowUnix + qint64(durationSec - pos + 0.5);
        else
            a.startUnix = nowUnix - qint64(pos);
    }

    if (!item.imdbId.isEmpty()) {
        // An episode's id is "ttShow:season:episode"; IMDb's title URL wants the show id alone.
        const QString tt = item.imdbId.section(QLatin1Char(':'), 0, 0);
        if (tt.startsWith(QLatin1String("tt")))
            a.buttons << qMakePair(QStringLiteral("View on IMDb"),
                                   QStringLiteral("https://www.imdb.com/title/%1/").arg(tt));
    }
    if (a.buttons.size() < kMaxButtons)
        a.buttons << qMakePair(QStringLiteral("Get EverythingBox"), QString::fromLatin1(AppBrand::kSiteUrl));
    return a;
}

Presence::Activity Presence::idle(qint64 sessionStartUnix)
{
    Activity a;
    a.valid      = true;
    a.type       = kPlaying;
    a.details    = QStringLiteral("Browsing");
    a.largeImage = QStringLiteral("logo");
    a.largeText  = QString::fromLatin1(AppBrand::kName);
    a.startUnix  = sessionStartUnix;
    a.buttons << qMakePair(QStringLiteral("Get EverythingBox"), QString::fromLatin1(AppBrand::kSiteUrl));
    return a;
}
