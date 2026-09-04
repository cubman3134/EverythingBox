// THE TRACKER SEAM (issue #156, increment 1) — one interface for "an account somewhere else that remembers
// how far through a series you are". AniList is the first and only implementation this increment;
// MyAnimeList and Kitsu are later increments behind exactly this shape, and their enum values are RESERVED
// below rather than invented later, so a stored token string minted today cannot collide with one of theirs.
//
// PURE. No QObject, no network, no ini, no GUI — deliberately, because AniListTracker IS a QObject and a
// probe that had to moc this header could not link against Qt6::Core alone. Everything here is a struct, an
// enum, a key spelling or an abstract method; the wire formats live in TrackerRules (also pure) and the
// socket lives in AniListTracker.
//
// WHY THE CALLBACKS ARE std::function AND NOT SIGNALS. The interface is implemented by a QObject, but it is
// CONSUMED by MainWindow through a base-class pointer. A signal cannot be declared on a non-QObject base, and
// making this base a QObject would force every future tracker into single inheritance from it. The Trakt read
// layer already answers its callers this way (fetchMyShowsCalendar), and it carries the same warning: a
// callback may NEVER ARRIVE if the tracker is destroyed with a request in flight. Own the lifetime; never
// park UI state on the callback always firing.
#pragma once
#include <QString>
#include <QVector>
#include <functional>

namespace tracker
{
    // ---- which service ---------------------------------------------------------------------------------
    // The values are STABLE and are what a stored key spells. MyAnimeList and Kitsu are reserved here, not
    // added later: the per-item link store and the offline queue are both keyed by this token, and a service
    // that arrives with a token some previous build could have written under a different meaning would read
    // another tracker's links as its own.
    enum class Id
    {
        AniList = 0,
        MyAnimeList = 1,   // RESERVED — increment 2. Nothing implements it yet.
        Kitsu = 2,         // RESERVED — increment 3. Nothing implements it yet.
    };

    // The stable token for `id` — what every key, every queue row and every stored link is spelled with.
    // Returns "" for a value outside the enum, and callers treat "" as "no tracker", never as a default:
    // a link filed under the empty token would be readable by whatever the next build made token 0 mean.
    inline QString idToken(Id id)
    {
        switch (id)
        {
            case Id::AniList:     return QStringLiteral("anilist");
            case Id::MyAnimeList: return QStringLiteral("mal");
            case Id::Kitsu:       return QStringLiteral("kitsu");
        }
        return QString();
    }

    // What a tracker entry is ABOUT. AniList's `MediaType` has exactly these two members, and every other
    // tracker in the issue splits the same way, so the seam carries the distinction rather than each
    // implementation guessing it from the item.
    enum class Kind { Anime, Manga };

    inline QString kindToken(Kind k)
    {
        return k == Kind::Manga ? QStringLiteral("manga") : QStringLiteral("anime");
    }

    // The reading/watching state of a list entry. AniList's MediaListStatus, minus REPEATING, which this
    // increment never WRITES (nothing in the app means "I am rewatching") but must be able to READ back
    // without mistaking it for something it would then overwrite — see `Repeating`.
    enum class Status { Current, Planning, Completed, Dropped, Paused, Repeating };

    // ---- what a search found ---------------------------------------------------------------------------
    struct Match
    {
        QString mediaId;    // the tracker's own id, as a string — AniList's is an int, MAL's is an int,
                            // Kitsu's is a string. One type at the seam.
        QString title;      // the best display title the tracker gave (English where present, else romaji)
        QString altTitle;   // the other one, for the picker's second line; "" when the tracker gave only one
        int     year = 0;   // 0 = the tracker gave no start year
        Kind    kind = Kind::Anime;
        int     totalUnits = 0;  // episodes or chapters; 0 = unknown/ongoing. Drives the COMPLETED rule.
        QString coverUrl;   // "" when none
    };

    // ---- what the tracker currently holds for one linked item ------------------------------------------
    struct Entry
    {
        QString mediaId;
        bool    exists = false;   // false = the account has no list entry for this media at all. Distinct
                                  // from progress==0: "not on my list" and "on my list, unread" reconcile
                                  // identically today but say different things to the user, and only one of
                                  // them means a push will CREATE a row.
        int     progress = 0;     // episodes watched / chapters read
        Status  status = Status::Current;
        int     score = 0;        // 0..100 (AniList's POINT_100 raw). 0 = unrated.
        int     totalUnits = 0;   // as Match::totalUnits
    };

    // ---- one progress event the app wants pushed --------------------------------------------------------
    // Produced by the app's OWN completion paths (a chapter's last page; a video stopping past the watched
    // threshold) and consumed by the debounce + the offline queue. It is a value type on purpose: it is
    // written to disk verbatim and replayed after a restart, so it may not hold a pointer or an index.
    struct Update
    {
        QString itemKey;    // the app's marks key for the series — what a link is stored against
        QString mediaId;    // the linked tracker id. Empty is not queueable: no link, no push.
        Kind    kind = Kind::Anime;
        int     unit = 0;   // the episode/chapter number just finished. 1-based.
        bool    completes = false;  // this was the LAST unit -> status COMPLETED
        bool    hasScore = false;   // the app has a rating for this item...
        int     score = 0;          // ...and this is it, 0..100. Never sent when hasScore is false: a 0
                                    // score on AniList is not "unrated", it is "rated zero", and pushing it
                                    // would wipe a rating the user set in their tracker.
        qint64  atMs = 0;   // when the app observed it (monotonic-ish wall clock, ms). Drives the debounce
                            // and survives to disk so a replayed queue keeps its ordering.
    };

    // ---- where the state lives --------------------------------------------------------------------------
    //
    // Three families, and the split decides three different things: what SYNCS, what a settings Discard may
    // revert, and what is a secret.
    //
    //   settingsKeyPrefix()  what the user TYPES: the client id, the client secret, and the tokens the OAuth
    //                        exchange produced. DEVICE-LOCAL — this is the secrets carve-out, and it is the
    //                        whole reason this family is separate from Trakt's (trakt/clientId DOES sync).
    //                        The issue asks for the credentials to stay on the device that was linked, and a
    //                        synced settings bundle is a zip on a third party's disk.
    //   stateKeyPrefix()     what PUSHING writes: the offline queue, the per-item debounce stamps, the last
    //                        error. Device-local for the ScrobbleQueue reasons — merging two devices' queues
    //                        submits the same progress twice — and excluded from the settings transaction,
    //                        because a chapter finished while a settings panel is open must not show up as
    //                        "1 setting changed" and must not be thrown away by Discard.
    //   linkKeyPrefix()      which tracker entry an item IS. SYNCED, per-item, through the CloudMerge
    //                        document — the inverse classification of the two above, and deliberately so: a
    //                        link is a property of the CONTENT ("this shelf row is AniList 30002"), it is
    //                        expensive for the user to re-establish (a prompt per item), and it is not a
    //                        secret. Riding the heavy settings bundle instead would flip the stateHash on
    //                        every link and re-upload the whole zip.
    //
    // Distinct TOP-LEVEL groups rather than "tracker/state/…" so no tracker token or profile id can collide
    // with the discriminator — the same reasoning, and the same shape, as Scrobble::stateKeyPrefix().
    inline QString settingsKeyPrefix() { return QStringLiteral("tracker/"); }
    inline QString stateKeyPrefix()    { return QStringLiteral("trackerstate/"); }
    inline QString linkKeyPrefix()     { return QStringLiteral("trackerlink/"); }

    // The five keys one tracker's credentials occupy, spelled once. Everything that reads or carves them out
    // goes through these rather than a literal, so a later tracker cannot be added to one list and missed in
    // another — which is how trakt/calendarCache came to be excluded from the settings transaction and NOT
    // from the sync bundle for a whole release.
    inline QString credKeyPrefix(Id id)   { return settingsKeyPrefix() + idToken(id) + QLatin1Char('/'); }
    inline QString clientIdKey(Id id)     { return credKeyPrefix(id) + QStringLiteral("clientId"); }
    inline QString clientSecretKey(Id id) { return credKeyPrefix(id) + QStringLiteral("clientSecret"); }
    inline QString accessKey(Id id)       { return credKeyPrefix(id) + QStringLiteral("access"); }
    inline QString refreshKey(Id id)      { return credKeyPrefix(id) + QStringLiteral("refresh"); }
    inline QString expiryKey(Id id)       { return credKeyPrefix(id) + QStringLiteral("expiry"); }

    // The OAuth artefacts, for every tracker including the two that are only reserved. They are matched as
    // EXACT leaves rather than by a "tracker/" prefix for the reason trakt/access is: the prefix would also
    // swallow the client id and secret, which the user TYPES and which a settings Discard must be able to
    // put back.
    inline bool isTokenKey(const QString& key)
    {
        for (Id id : { Id::AniList, Id::MyAnimeList, Id::Kitsu })
            if (key == accessKey(id) || key == refreshKey(id) || key == expiryKey(id)) return true;
        return false;
    }

    // Every key that must never ride a sync bundle. What CloudSync::isDeviceLocalKey asks. NOTE that
    // linkKeyPrefix() is deliberately NOT here — it is per-item-synced instead, and probe_cloudmerge pins
    // both halves of that split so a later edit cannot quietly move the links into the secrets family or
    // (much worse) the credentials into the synced one.
    inline bool isDeviceLocalKey(const QString& key)
    {
        return key.startsWith(settingsKeyPrefix()) || key.startsWith(stateKeyPrefix());
    }

    // The half a settings transaction must not snapshot or revert. What SettingsTxn::inScope asks. The
    // typed credentials are NOT here on purpose: pasting the wrong client secret and pressing Discard has to
    // put the old one back, exactly as it does for Trakt. The TOKENS are — linking an account from inside a
    // settings visit is the ordinary route, and a Discard on the way out must not un-link it.
    inline bool isBackgroundStateKey(const QString& key)
    {
        return key.startsWith(stateKeyPrefix()) || isTokenKey(key);
    }

    // ---- the #81 BuiltinSecrets follow-up ---------------------------------------------------------------
    // The zero-config version of this feature embeds the app's own AniList client id + secret the way
    // ScreenScraper's dev credentials are embedded, and falls back to the user's typed pair. These are the
    // SLOT NAMES that change is a one-line edit against, written now so the later change does not have to
    // invent them and does not have to migrate anything:
    //
    //   native/secrets/anilist.secrets     EB_ANILIST_ID  / EB_ANILIST_SECRET
    //   BuiltinSecrets.h.in                kAniList_Id_A / kAniList_Id_B  (+ _ALen / _BLen)
    //                                      kAniList_Secret_A / kAniList_Secret_B (+ _ALen / _BLen)
    //
    // The one-line change is in AniListTracker::clientId()/clientSecret(): prefer the user's typed value,
    // else the embedded slot. Nothing else moves — the token exchange, the storage and the carve-outs are
    // already written in terms of those two accessors rather than the Settings keys.
    inline QString builtinSecretIdSlot()     { return QStringLiteral("kAniList_Id"); }
    inline QString builtinSecretSecretSlot() { return QStringLiteral("kAniList_Secret"); }

    // ---- the interface ----------------------------------------------------------------------------------
    // Implemented by AniListTracker. Every method is callable when the tracker is not configured or not
    // connected: that is not an error, it is "the feature is off", and each one answers with the empty/false
    // result rather than refusing.
    class Tracker
    {
    public:
        virtual ~Tracker() = default;

        virtual Id id() const = 0;
        virtual QString displayName() const = 0;   // "AniList" — what a prompt and a settings row say

        virtual bool configured() const = 0;   // a client id + secret are present
        virtual bool connected() const = 0;    // an access token is stored

        // Search the tracker for `title` (optionally narrowed by `year`, 0 = any) of `kind`. Answers with an
        // EMPTY list — never an error — when the tracker is off, so a caller's "no matches" path covers both.
        virtual void search(const QString& title, int year, Kind kind,
                            std::function<void(QVector<Match>)> cb) = 0;

        // Read the account's list entry for `mediaId`. ok=false means "could not ask" (off, or the request
        // failed); an entry with exists=false means "asked, and the account has no row for it".
        virtual void fetchEntry(const QString& mediaId, Kind kind,
                                std::function<void(bool ok, Entry)> cb) = 0;

        // Push one progress update. Debounced and queued INSIDE the implementation — a caller fires this on
        // every completion event and never has to think about rate limits or being offline.
        virtual void pushProgress(const Update& u) = 0;

        // Try to deliver anything the queue is still holding. Called on connect and on a network return; a
        // no-op when the queue is empty or the tracker is off.
        virtual void flushQueue() = 0;
    };
}
