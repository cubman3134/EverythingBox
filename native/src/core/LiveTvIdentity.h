// THE DURABLE NAME OF A LIVE TV CHANNEL (issue #203, the Live TV half).
//
// THE BUG. A favourited IPTV channel was filed under `"livetv:" + <stream url>`, with the same url again in
// its path, and a channel added to a playlist filed the same string as its entry id. `favorites/` and
// `playlists/` are both per-item SYNCED stores, so that url rode the CloudMerge document to every device on
// the account — and an IPTV url is the one this project has already ruled "routinely embeds provider
// credentials", commonly in the PATH (`…/live/<user>/<pass>/<id>.ts`), which StoredUrl deliberately does not
// touch. 02a18bd contained it by refusing to serialise or accept ANY `livetv:` row. That closed the leak and
// cost the feature: a starred channel stopped syncing.
//
// WHAT A CHANNEL ACTUALLY IS. Not a url. A url is where ONE provider serves it today, with that provider's
// credential in it, and it changes when the credential rotates. The thing that is the same channel on every
// provider and every device is its EPG id — `tvg-id` — which is credential-free, provider-independent, and
// already the field the whole guide keys on (XmltvGuide, liveTvNowNextByTvgId). So:
//
//     tvg-id present  ->  livetv:<tvg-id>
//     tvg-id absent   ->  livetv:name:<normalised name>
//
// and the url is looked up FROM the id at open time, never stored as the identity. No hashes: an id a human
// can read in the ini is an id a human can debug, and every one of these is greppable next to the channel it
// names.
//
// THE NAME RULE, AND WHY IT IS A PROPERTY OF THE LIST RATHER THAN OF ONE ENTRY. Normalising is trim, collapse
// internal whitespace, case-fold. The quality tag is the part that cannot be decided from one entry: a
// provider that lists BOTH "CNN" and "CNN HD" is listing one channel twice, so the tag is dropped and the two
// become one identity (the collision rule below then picks which url that identity resolves to). A provider
// that lists ONLY "CNN HD" is naming its channel that, and dropping the tag there would invent a name nothing
// carries — so the name is kept whole. Hence idsFor() takes the whole list: it cannot be a per-entry function.
//
// COLLISIONS. Two entries deriving the same id (a duplicate tvg-id, or the pair above) are not an error and
// neither row is hidden — the channel list is a view of the provider's playlist, not an identity store, and
// silently dropping a channel a user can see in their provider's app is a worse bug than a shared id.
// Resolution keeps the FIRST in M3U order, which is the provider's own preference order. collisionsOut lets
// the caller log it once per load; it names ids only, so a log line can never carry a url.
//
// THE WIRE SPELLING. A row this device has not been able to re-identify yet (its source is not configured
// here, or it has simply never been loaded) still holds its raw url locally — it is the only thing that can
// play it, and a favourite that stops working is a worse outcome than the leak. It just never leaves in that
// spelling: wireId() gives it the credential-free name spelling, which is both safe to send and, on a peer
// that HAS the channel, a real identity that resolves. See CloudMerge's playlist arm for the rule that stops
// one overwriting a peer's playable copy.
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

namespace LiveTvIdentity
{
    // One channel as parsed from an M3U, in the only three fields identity cares about. Deliberately NOT
    // M3uEntry: this header is QtCore-only and is linked into probes (and into CloudMerge) that have no
    // business pulling media/StreamResolver.h.
    struct Channel
    {
        QString tvgId;    // tvg-id   — the EPG id, when the entry carries one
        QString tvgName;  // tvg-name — the canonical channel name, when the entry carries one
        QString title;    // the #EXTINF display title (always present)
        QString url;      // the playable stream
        // The name the identity rule uses: the canonical one when there is one, else what is on screen.
        QString name() const { return tvgName.isEmpty() ? title : tvgName; }
    };

    // trim + collapse internal whitespace + case-fold. Empty in, empty out.
    QString normalizedName(const QString& raw);

    // `raw` with a single trailing quality token removed ("cnn hd" -> "cnn"), or `raw` unchanged when the last
    // token is not one. Input is expected already normalised. The token may be wrapped in ()/[] ("cnn (hd)").
    // The recognised set is deliberately short and closed — hd, fhd, uhd, sd, hq, 4k, 8k — because every entry
    // added to it silently merges two identities somebody may be relying on.
    QString withoutQualityTag(const QString& normalized);

    bool isLiveTvId(const QString& id);            // "livetv:…"

    // A LEGACY identity: a `livetv:` id that is still a url. This is the one that must not leave the device —
    // and the test is applied to the name-spelling's payload too, because a channel whose TITLE is a url would
    // otherwise smuggle one out through the fallback.
    bool isCredentialShaped(const QString& id);

    QString idForTvgId(const QString& tvgId);          // "livetv:<trimmed tvg-id>"; empty in, empty out
    QString idForName(const QString& channelName);     // "livetv:name:<normalised>"; empty name -> empty

    // THE RULE, over a whole source's channel list. ids[i] is entry i's identity, in the input's own order and
    // one-for-one with it (never shorter: a row with no identity is a row nothing can star). `collisionsOut`,
    // when given, receives each id that more than one entry derived, once, in first-seen order.
    QVector<QString> idsFor(const QVector<Channel>& channels, QStringList* collisionsOut = nullptr);

    // The url `id` names in this list, or empty when no entry derives it. FIRST match in list order wins.
    QString urlFor(const QVector<Channel>& channels, const QString& id);

    // The credential-free spelling an un-re-identified row travels under: the name rule applied to whatever
    // human-readable title the row kept. A row with no title at all still gets a name rather than nothing, so
    // the merge below always has something to match on.
    QString wireId(const QString& title);

    // WHAT A `recent/` ROW RECORDS FOR A CHANNEL (issue #245; the rule was #203's, this is it as a function).
    //
    // `resumeKey` is what the open was keyed by and `streamUrl` is the link that was just minted from it.
    // The answer is the KEY whenever the key is a channel identity this device can re-resolve, and the url
    // otherwise. Two reasons, and neither is cosmetic:
    //
    //   * `recent/` is a SYNCED per-item store. #200's scrub takes the QUERY off a stored url and cannot
    //     touch its PATH — and an IPTV url is the one this project has already ruled "routinely embeds
    //     provider credentials", commonly as `…/live/<user>/<pass>/<id>.ts`. Recording the url therefore
    //     puts a credential on the CloudMerge document to every device on the account.
    //   * The url is a fact about the provider's credential TODAY. Recording it makes Continue Watching stop
    //     working the moment that credential rotates, when the identity would still have resolved.
    //
    // A LEGACY `livetv:<url>` KEY IS DELIBERATELY NOT CLAIMED. Such a row (LiveTvMigrate's third outcome)
    // has nothing else to be re-opened from, so it keeps replaying its url exactly as it always did; the
    // caller's own dispatch refuses it for the same reason. isCredentialShaped is what tells the two apart.
    //
    // It lives HERE, pure and QtCore-only, rather than inline at the write site, because the write site is
    // MainWindow — which links nothing headlessly — and this one boolean is the whole distance between a
    // synced store that holds a credential and one that does not. probe_cloudmerge §40 pins it.
    QString recentPathFor(const QString& resumeKey, const QString& streamUrl);

    // ---- LOCAL LEAF KIND (declared with the feature that stamps it — see browse/LeafRoute.h) -------------
    // A STARRED CHANNEL SHOWN AS A BROWSE ROW (issue #244): the themed Favourites shelf's channel rows carry
    // this prefix followed by the channel identity, and channelKeyOf reads it back. Keyed, and carrying NO
    // url on purpose — the whole point of #203 is that the url is minted at open from this device's sources
    // — which is why it needs a route of its own rather than the generic "open this row's file" one.
    //
    // The spelling is NOT "livetv:", the identity's own prefix, and not "livetv" either, which is the mime
    // the CHANNEL LIST's rows carry (those DO have a url and open through it). A prefix that matched either
    // would re-route a surface this issue is not about.
    inline const char* kLiveTvChannelPrefix = "livetvchan:";

    // The key a keyed mime carries: EVERYTHING after the prefix, never a section(':') — a channel identity
    // is itself full of colons ("livetv:name:bbc one"), so a split would truncate every one of them into a
    // different channel's id. The same rule, and the same reason, as browse::jellyfinKeyOf.
    inline QString channelKeyOf(const QString& mime, const char* prefix)
    {
        const QString p = QString::fromLatin1(prefix);
        return mime.startsWith(p) ? mime.mid(p.size()) : QString();
    }
}
