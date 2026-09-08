// THE JELLYSEERR CREDENTIAL, PER PROFILE, DEVICE-LOCAL (issue #109).
//
// One service, not a list: a household runs one Jellyseerr in front of one *arr stack, and a second one
// would be a second household's. That is the difference from JellyfinServerStore, which holds N servers
// because "my box plus the one a friend shares with me" is an ordinary Jellyfin setup and there is no
// equivalent shape here.
//
// ==========================================================================================================
// THE KEY IS A CREDENTIAL AND IT NEVER LEAVES THIS MACHINE
// ==========================================================================================================
// The store writes under the "jellyseerr/" ini prefix, which CloudSync::isDeviceLocalKey carves OUT of the
// synced settings bundle. A synced bundle is a zip in somebody's Drive folder; a Jellyseerr API key in it is
// a standing grant over that household's whole acquisition pipeline sitting on a third party's disk.
// probe_cloudmerge pins the carve-out and probe_requests byte-scans a fixture key across everything this
// feature writes.
//
// The key is read at request-build time and put into a header (jellyseerr::apiKeyHeader). It is never
// logged, never rendered, and never returned to anything that could put it in a message — JellyseerrClient
// has no call to QNetworkReply::errorString() for exactly that reason.
//
// THE URL RIDES WITH IT, for the same reason the OPDS and IPTV stores keep theirs device-local: it is
// routinely a private LAN address that means nothing on another machine, and it is half of what identifies
// the credential.
#pragma once
#include <QString>
#include <functional>

struct JellyseerrConfig
{
    QString url;      // the service root, e.g. https://requests.example.com — no trailing slash needed
    QString apiKey;   // DEVICE-LOCAL — never synced, never logged, never shown
    // The explicit choice rather than a silent downgrade: the key rides a header on EVERY call, so plain
    // HTTP is refused by jellyseerr::checkUrl unless the user has said otherwise, and the question is asked
    // BEFORE the key is stored.
    bool    allowPlainHttp = false;

    bool configured() const { return !url.trimmed().isEmpty() && !apiKey.isEmpty(); }
};

namespace JellyseerrStore
{
    JellyseerrConfig get();                 // for the active profile; a default-constructed value when unset
    bool             isConfigured();

    void save(const JellyseerrConfig& c);
    // Forget it. Drops the key with the url — there is no "switch off" that keeps a credential around,
    // because there is nothing here a user would want to hide temporarily the way a friend's 8,000 films
    // are hidden.
    void clear();

    // A ONE-LINE status sentence for both settings builders. It names NO url, NO key and NO account — it
    // says whether a service is set up and nothing else, which is the whole of what a settings line has any
    // business saying about a credential.
    QString statusLine();

    // Fired after every mutation, so a settings surface's status line and the Request action's gate both
    // catch up. Unset in headless probes (fires nothing).
    void setChangeHook(std::function<void()> hook);

#ifdef EB_REQUESTS_TEST_SEAM
    // Test-only ini redirect, the same macro and the same rule as JellyfinServerStore's: without the define
    // the symbol does not exist, so a production call is a compile error rather than a silent process-wide
    // redirect. Load-bearing here for a second reason — this store holds a KEY, and a probe run against the
    // app's real ini would write a fixture credential into the user's own settings file.
    void setIniPathForTesting(const QString& path);
#endif
}
