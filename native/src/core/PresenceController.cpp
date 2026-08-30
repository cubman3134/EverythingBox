#include "PresenceController.h"
#include "PresenceTransport.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QtGlobal>

namespace {

// The player's reported position jitters by a fraction of a second between ticks. Recomputing the end
// instant from it verbatim would flicker endUnix by a second at a time, so the card would "change" on most
// ticks and a frame would go out every kFloorMs for ever - burning Discord's rate limit to redraw something
// no human can see move. Anything inside this band is the SAME card.
//
// Two seconds is chosen so that real drift still gets through: at 2x playback the end instant moves a second
// per real second and crosses the band almost immediately, and a seek moves it by minutes. Only jitter is
// absorbed, and jitter does not accumulate because it oscillates around the true value.
constexpr qint64 kTimestampSlackSec = 2;

bool sameCard(const Presence::Activity& a, const Presence::Activity& b)
{
    if (a.valid != b.valid) return false;
    Presence::Activity x = a, y = b;
    x.startUnix = y.startUnix = 0;
    x.endUnix   = y.endUnix   = 0;
    if (!(x == y)) return false;
    return qAbs(a.startUnix - b.startUnix) <= kTimestampSlackSec
        && qAbs(a.endUnix   - b.endUnix)   <= kTimestampSlackSec;
}

} // namespace

PresenceController::PresenceController(QObject* parent)
    : QObject(parent)
    , sessionStart_(QDateTime::currentSecsSinceEpoch())
{
    floor_.setSingleShot(true);
    floor_.setInterval(kFloorMs);
    connect(&floor_, &QTimer::timeout, this, [this] {
        if (!pending_) return;
        pending_ = false;
        rebuild();
    });
}

void PresenceController::setTransport(PresenceTransport* transport)
{
    transport_ = transport;
    // A transport arriving after the app has already opened something must be told what is showing, or the
    // card stays blank until the next track boundary.
    anythingSent_ = false;
    rebuild();
}

void PresenceController::setItem(const Presence::Item& item)
{
    item_ = item;
    // A new item's clocks belong to it, not to whatever was playing before. Without this reset, opening a
    // three-minute track while an hour-long film's duration is still held builds a countdown from the film.
    position_ = 0.0;
    duration_ = 0.0;
    paused_   = false;
    rebuild();
}

void PresenceController::clearItem()
{
    item_ = Presence::Item{};
    position_ = duration_ = 0.0;
    paused_ = false;
    rebuild();
}

void PresenceController::setPosition(double seconds) { position_ = seconds; rebuild(); }
void PresenceController::setDuration(double seconds) { duration_ = seconds; rebuild(); }
void PresenceController::setPaused(bool paused)      { paused_   = paused;  rebuild(); }

void PresenceController::settingsChanged()
{
    rebuild();
    emit statusChanged();
}

void PresenceController::rebuild()
{
    if (!transport_) return;

    // What SHOULD be showing, given the gate.
    Presence::Activity next;
    if (item_.kind != Presence::Kind::None && Settings::discordShows(item_.kind))
        next = Presence::build(item_, position_, duration_, paused_,
                               QDateTime::currentSecsSinceEpoch());
    else if (item_.kind == Presence::Kind::None && Settings::discordEnabled() && Settings::discordBrowsing())
        next = Presence::idle(sessionStart_);
    // else: next stays invalid -> clear

    if (anythingSent_ && sameCard(next, lastSent_)) return;   // nothing changed: send nothing at all

    if (floor_.isActive()) { pending_ = true; return; }   // too soon: coalesce
    deliver(next);
    floor_.start();
}

void PresenceController::deliver(const Presence::Activity& next)
{
    if (next.valid) transport_->setActivity(next);
    else            transport_->clearActivity();
    lastSent_     = next;
    anythingSent_ = true;
    emit statusChanged();
}

QString PresenceController::statusLine() const
{
    if (!Settings::discordEnabled())
        return QCoreApplication::translate("PresenceController", "Discord presence is off.");
    if (!transport_ || !transport_->connected())
        return QCoreApplication::translate("PresenceController",
            "Discord isn't running - your status will appear as soon as you start it.");
    if (lastSent_.valid && !lastSent_.details.isEmpty())
        return QCoreApplication::translate("PresenceController",
            "Connected to Discord - showing \342\200\234%1\342\200\235.").arg(lastSent_.details);
    return QCoreApplication::translate("PresenceController", "Connected to Discord.");
}
