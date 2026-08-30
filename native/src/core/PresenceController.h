// THE PRESENCE ORCHESTRATOR — what is showing, and when a new card is owed.
//
// It holds four facts (the item, the position, the duration, whether playback is paused), rebuilds the card
// from Presence::build whenever one of them moves, and sends it ONLY IF IT DIFFERS from the card last sent.
//
// THAT COMPARISON IS THE WHOLE THROTTLE. Discord accepts five updates per twenty seconds. A per-second
// position tick would exceed that in four seconds and the card would freeze — except that a second of
// ordinary playback moves the position and the clock by the same amount, so the end instant is unchanged and
// build() returns a card equal to the last one. Nothing is sent. What DOES change — a seek, a pause, a track
// boundary — is genuinely rare, and kFloorMs coalesces even those.
//
// The host tells it five things and reads one back; every hook point in the app is one of the setters below.
#pragma once
#include "Presence.h"

#include <QObject>
#include <QString>
#include <QTimer>

struct PresenceTransport;

class PresenceController : public QObject
{
    Q_OBJECT
public:
    explicit PresenceController(QObject* parent = nullptr);

    // Takes no ownership: the host owns the transport (MainWindow parents the DiscordPresence to itself),
    // exactly as Scrobbler::setProvider is used.
    void setTransport(PresenceTransport* transport);

    void setItem(const Presence::Item& item);  // something opened
    void clearItem();                          // ...and closed; falls back to the browsing card
    void setPosition(double seconds);
    void setDuration(double seconds);
    void setPaused(bool paused);

    // Re-read the settings gate and act on it now. Called from both settings builders on every toggle, so a
    // category switched off mid-film clears the card rather than waiting for the next boundary.
    void settingsChanged();

    // The one line both settings surfaces show. Never names anything the user has silenced.
    QString statusLine() const;

signals:
    void statusChanged();

private:
    void rebuild();     // build -> compare -> send, or defer to the floor timer
    void deliver(const Presence::Activity& next);

    PresenceTransport* transport_ = nullptr;
    Presence::Item     item_;
    double             position_ = 0.0;
    double             duration_ = 0.0;
    bool               paused_   = false;
    qint64             sessionStart_ = 0;   // when this app run began; the browsing card counts from it

    Presence::Activity lastSent_;
    bool               anythingSent_ = false;
    QTimer             floor_;             // coalesces changes that arrive faster than kFloorMs
    bool               pending_ = false;

    // Discord allows five updates per twenty seconds. Four seconds between sends leaves headroom for the
    // occasional burst (a seek immediately followed by a pause) without ever approaching the ceiling.
    static constexpr int kFloorMs = 4000;
};
