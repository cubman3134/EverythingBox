// A DOWNLOAD THAT CAN ASK FOR ITS LINK AGAIN (issue #437) — the #224 re-mint recipe, written into a
// DownloadJob's sourceRef instead of its url.
//
// An add-on download's url is a debrid link or a stream link, and it is exactly the credential-carrying
// thing #200 took out of Recents and #224 learned to mint afresh for Continue Watching. The recipe #224
// stores for a Recents row — route, type, addon id, item id — is credential-free by construction (see
// MainWindow.cpp's applyRemintRecipe / remintableId) and is everything a resolve needs. So a download that
// has one keeps THAT, not the link: queue.json holds four ids, and a resume asks the same source for a
// fresh link through the same two calls remintAndOpen makes (resolveStream / resolveStreamByImdb).
//
// THIS IS NOT A SECOND RE-MINT SYSTEM. The recipe is computed by applyRemintRecipe, routed by
// RecentStore::reopenFor and resolved by AddonManager, exactly as for a Recents row; this header only spells
// the four fields as one opaque ref string and reads them back. It knows nothing about addons.
//
// THE SHAPE: "rm1:<route>:<type>:<addonId>:<itemId>", every field percent-encoded (so an item id such as
// "ttShow:1:2" cannot add a separator). Distinct by prefix from every other ref family (Jellyfin's
// "jf:…", Subsonic's qualified id, Audiobookshelf's file ref), so DownloadManager can tell which minter owns
// a ref without asking either of them.
//
// Header-only, and nothing from the addon layer: probe_jfdownload drives it with none.
#pragma once

#include "DownloadManager.h"   // DownloadJob

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace DownloadRecipe
{
struct Recipe
{
    QString route;    // "direct" | "imdb" — RecentItem::sourceRoute
    QString type;     // RecentItem::sourceType
    QString addonId;  // RecentItem::sourceAddonId (ignored by the imdb route, recorded anyway)
    QString itemId;   // RecentItem::sourceItemId
};

inline QLatin1String prefix() { return QLatin1String("rm1:"); }

inline bool isRef(const QString& sourceRef) { return sourceRef.startsWith(prefix()); }

namespace detail
{
// The last line of the "no credential in a ref" rule. applyRemintRecipe has already refused a url-shaped id
// (remintableId); this refuses one again at the point where the ref is about to be PERSISTED, so a future
// caller that skipped the recipe builder still cannot write a link into queue.json through this door.
inline bool credentialFree(const QString& v)
{
    return !v.contains(QLatin1Char('?')) && !v.contains(QLatin1Char('#'))
        && !v.contains(QLatin1String("//"));
}
inline QString enc(const QString& v) { return QString::fromLatin1(QUrl::toPercentEncoding(v)); }
inline QString dec(const QString& v) { return QUrl::fromPercentEncoding(v.toLatin1()); }
} // namespace detail

// The ref for a COMPLETE recipe, else an empty string (the job then keeps its url, sealed — see UrlAtRest.h).
// Complete means what RecentStore::reopenFor means: a route it knows, a type and an item id; and for the
// direct route an addon, because that route asks exactly one.
inline QString encode(const Recipe& r)
{
    if (r.route != QLatin1String("direct") && r.route != QLatin1String("imdb")) return QString();
    if (r.type.isEmpty() || r.itemId.isEmpty()) return QString();
    if (r.route == QLatin1String("direct") && r.addonId.isEmpty()) return QString();
    for (const QString& f : { r.type, r.addonId, r.itemId })
        if (!detail::credentialFree(f)) return QString();
    return prefix() + detail::enc(r.route) + QLatin1Char(':') + detail::enc(r.type) + QLatin1Char(':')
         + detail::enc(r.addonId) + QLatin1Char(':') + detail::enc(r.itemId);
}

inline bool decode(const QString& sourceRef, Recipe* out)
{
    if (!isRef(sourceRef)) return false;
    const QStringList parts = sourceRef.mid(prefix().size()).split(QLatin1Char(':'));
    if (parts.size() != 4) return false;
    Recipe r;
    r.route   = detail::dec(parts[0]);
    r.type    = detail::dec(parts[1]);
    r.addonId = detail::dec(parts[2]);
    r.itemId  = detail::dec(parts[3]);
    if (encode(r) != sourceRef) return false;   // not one this build wrote: refuse rather than guess
    if (out) *out = r;
    return true;
}

// THE SITE'S HALF, in one place so a probe can hold it to account: a job whose resolve left a complete
// recipe keeps the recipe and gives up its url. The url just resolved is not thrown away — it becomes the
// job's `mintedUrl`, used by the first transfer and never persisted (see DownloadJob). Returns whether the
// job was bound; false leaves it exactly as it was (a plain job, whose url DownloadManager seals at rest).
inline bool bindJob(DownloadJob& j, const Recipe& r)
{
    const QString ref = encode(r);
    if (ref.isEmpty()) return false;
    j.sourceRef = ref;
    j.mintedUrl = j.url;
    j.url.clear();
    return true;
}
} // namespace DownloadRecipe
