// THE PRESENCE SEAM — the whole of what a presence service has to be able to do.
//
// Three verbs, because there are only three things the orchestrator ever wants: show this card, show nothing,
// and tell me whether you are reachable so the settings surface can say so. Everything that DECIDES what the
// card says lives in Presence.h; everything that decides WHEN lives in PresenceController.
//
// NOT A QObject, for ScrobbleProvider.h's reason: the orchestrator drives it directly through ordinary calls,
// making it a QObject would buy signals nobody wants, and it would cost probe_presence the ability to
// substitute a two-line recording fake.
#pragma once
#include "Presence.h"

struct PresenceTransport
{
    virtual ~PresenceTransport() = default;

    // Show this card. Called only when the card has actually CHANGED — the orchestrator does the comparing,
    // so an implementation may send unconditionally.
    virtual void setActivity(const Presence::Activity& activity) = 0;

    // Show nothing at all (the user switched presence off, or closed the last thing they had open).
    virtual void clearActivity() = 0;

    // Whether the service is reachable right now. FALSE IS ORDINARY: most users will not have Discord
    // running. It is a fact for the status line, never an error.
    virtual bool connected() const = 0;
};
