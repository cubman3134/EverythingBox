// WHAT THIS PROFILE HAS ASKED FOR (issue #109) — the rows the Requests shelf draws.
//
// ==========================================================================================================
// WHY WE KEEP OUR OWN LIST AT ALL
// ==========================================================================================================
// A request service can be asked for "the requests", but with an API key that answers with EVERYBODY's —
// the key is the household's, not a person's. The shelf the issue asks for is "the PROFILE's requests", and
// the only thing that knows which of a household's requests came from this profile on this device is this
// device. So the row is written HERE, at the moment of the press, and the service is asked only about its
// STATUS.
//
// That also makes the shelf honest when the service is unreachable: the rows are still there, each showing
// the last thing we were told, and the ones we could not refresh say "Status unknown" with a reason rather
// than silently claiming "pending".
//
// ==========================================================================================================
// A ROW HOLDS NO CREDENTIAL AND NO URL
// ==========================================================================================================
// It names a title, the ids it was requested by, the backend that was asked, and a status token. The
// service's address and its key live in JellyseerrStore and are read at request time. Nothing that could be
// a credential is ever written here — probe_requests byte-scans this store for the fixture key as an
// assertion, not as a claim.
//
// Written under the "requests/" ini prefix, which CloudSync::isDeviceLocalKey carves out of the synced
// bundle. DEVICE-LOCAL, and for the reason followsnap/ and openfail/ are: a row is a fact about a service
// THIS device is linked to. A peer with a different request service — or none — would show rows it can
// never refresh, complete with a "Ready to watch" badge for something that is not in its library, and the
// device that could correct them is not the device showing them.
#pragma once
#include "Requests.h"

#include <QString>
#include <QVector>
#include <functional>

namespace RequestStore
{
    QVector<requests::StoredRequest> list();          // active profile, newest-stored last
    int                              count();

    // Record a request that has just been ACCEPTED by a backend. De-duped by requests::MediaRef::key():
    // asking again for a title already on the shelf updates that row (its seasons union, its stamp, its
    // status) rather than growing a second one — two rows for one title is the shelf saying the request
    // happened twice, which is exactly the impression this feature must not give.
    void add(const requests::StoredRequest& row);

    // Update just the status of one row, from a refresh. A key with no row is a no-op: a refresh must never
    // create a row, because a row means "this profile asked for this" and a status fetch is not an ask.
    void setStatus(const QString& key, const QString& statusToken);

    void remove(const QString& key);
    bool get(const QString& key, requests::StoredRequest& out);

    // Fired after every mutation, so the shelf redraws. Unset in headless probes (fires nothing).
    void setChangeHook(std::function<void()> hook);

#ifdef EB_REQUESTS_TEST_SEAM
    void setIniPathForTesting(const QString& path);
#endif
}
