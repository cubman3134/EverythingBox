// ThemeShots — fetching and caching the pictures a theme registry entry advertises (issue #91).
//
// ThemeRegistry says WHICH urls an entry may be drawn from (screenshotUrls: https, the index's own host or
// one the user added, at most four). This is the half that touches the network and the disk, and it is one
// unit rather than a loop on each Appearance surface for the reason the rest of this feature is shared: the
// classic browser and the themed panel would otherwise each get their own idea of how big a screenshot may
// be and what counts as a picture, and only one of them would be pinned by anything.
//
// THREE RULES, all of which fail toward "no picture" and never toward "no theme":
//
//   * THE SIZE CAP IS APPLIED AS THE BYTES ARRIVE, not to the bytes that arrived. QNetworkAccessManager
//     buffers a whole response, so a cap tested after readAll() is a cap on an allocation that already
//     happened — the same reasoning (and the same mechanism: setReadBufferSize plus an abort on
//     downloadProgress) as the theme download loops. A response that DECLARES itself over budget is dropped
//     before its body arrives; one that declares nothing is stopped while it is read.
//
//   * THE BYTES DECIDE WHETHER THIS IS A PICTURE, never the url's extension and never the Content-Type
//     header — CoverFetch::isPicture, the product's one rule, the one the cover cache stores through
//     (#377/#387). An HTML error page served as "shot.png" with image/png on it is not art, and the
//     consequence of believing it would be an unreadable thumbnail that is also permanently "already
//     cached".
//
//   * NOTHING BLOCKS THE UI. Every fetch is asynchronous and each caller gets one callback; a row whose
//     picture has not landed (or never will) is the row it was before, and both surfaces draw it first and
//     patch it later.
//
// CACHING is the app's existing image cache (MetaCache), device-local, under a key per theme FOLDER and a
// role derived from the URL — so a registry that repoints a screenshot at a new url gets a new role and a
// fresh fetch, while an unchanged one is served from disk and never asked for twice. It is deliberately not
// synced: it is a thumbnail of someone else's screenshot, re-fetchable on any device in a second.
#pragma once
#include <QByteArray>
#include <QString>

#include <functional>

class QNetworkAccessManager;

namespace ThemeShots
{
// The MetaCache key one theme's pictures live under, and the role one picture lives under. Public because
// they are what "the cache key changes when the url changes" MEANS, and a rule stated only inside a network
// callback cannot be pinned by a probe.
//
// The key is namespaced ("themeshot:") rather than being the bare folder name: MetaCache's keys are media
// identities (addon ids, urls, paths) and a theme folder called "Trakt" must not land in another item's
// bundle. The role is a hash of the whole url, so two urls are two roles and the same url twice is one.
QString cacheKey(const QString& folder);
QString cacheRole(const QString& url);

// The locally cached file for this (folder, url), or empty when there is none. Goes through MetaCache's
// verified read-back, so a file stored before the bytes rule existed — or truncated since — is treated as
// absent rather than drawn as a broken image.
QString cachedPath(const QString& folder, const QString& url);

// May these bytes be cached and drawn? The size cap re-applied to what actually landed (the transfer abort
// is asynchronous: a reply that finishes in the same turn it crosses the budget can still hand back a body
// over it) AND the picture rule. Pure, so both halves are assertable without a socket.
bool acceptBytes(const QByteArray& body);

// Fetch one screenshot. `url` must already have been through ThemeRegistry::screenshotUrls — this function
// does not re-derive the host rule, and it is not a second door onto the network: it is only ever handed a
// url that rule produced.
//
// `done` is called exactly once, with the local file path, or EMPTY when the picture was refused or did not
// arrive. It fires directly (before returning) when the picture is already cached, so a surface that is
// drawing rows does not flicker through an empty state for a file it already has.
//
// A fetch already in flight for the same (folder, url) is not started twice; the later caller's `done` is
// invoked with what the first one lands (or empty). `nam` is the caller's manager and must outlive the
// request, which is the ordinary arrangement on both surfaces (MainWindow's docNam_, the browser's nam_).
void fetch(QNetworkAccessManager* nam, const QString& folder, const QString& url,
           std::function<void(const QString& localPath)> done);
}
