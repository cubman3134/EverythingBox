#include "CloudSync.h"
#include "DriveSyncBackend.h"  // the production backend CloudSync composes; static forwarders reach its statics
#include "ServerSyncBackend.h" // Increment C: the self-hosted transport, chosen by cloud/backend == "server"
#include "AppBrand.h"
#include "AppPaths.h"
#include "BrandMigration.h"  // the Drive lookups tolerate the previous brand until its flag is set
#include "Settings.h"   // deviceId() — stamped into meta.json (mdsync T4)
#include "SettingsTxn.h"  // a remote apply must close any open settings transaction (#26) — QtCore-only TU
#include "ProfilePasscode.h"  // isAttemptKey (header-only) — the passcode lockout is device-local, the hash syncs
#include "TraktSync.h"        // backfillKeyPrefix() — the per-profile import cursor family, device-local
#include "Scrobble.h"        // isDeviceLocalKey() - the #192 token/queue families, device-local
#include "PlayOnDevice.h"   // isDeviceLocalKey() - the #143 per-peer pairing tokens, device-local
#include "Tracker.h"         // isDeviceLocalKey()/linkKeyPrefix() - #156 straddles the carve-out both ways
#include "AddonRoster.h"     // issue #77: the roster's live keys leave the bundle; an old peer's snapshot is adopted
#include "AddonConfigKeys.h" // issue #77: an add-on's PASSWORD fields are device-local
#include "PerItemStores.h"   // #332: THE per-item-store prefix table, shared with SettingsTxn::inScope
#include "SyncCarry.h"       // #27: the relay buffer a switched-off category is carried through
#include <QMap>
#include <QSet>
#include <QSettings>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSysInfo>
#include <cstring>
#include "miniz.h"

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Pick the transport from config: cloud/backend == "server" selects the self-hosted object store, anything else
// (unset included) keeps Google Drive — so the default is byte-for-byte the pre-Increment-C behaviour. `owner`
// becomes the backend's parent; both ctors below hand it `this`.
static SyncBackend* makeConfiguredBackend(QObject* owner)
{
    if (store().value(QStringLiteral("cloud/backend")).toString() == QLatin1String("server"))
        return new ServerSyncBackend(owner);
    return new DriveSyncBackend(owner);
}

CloudSync::CloudSync(QObject* parent) : QObject(parent)
{
    backend_ = makeConfiguredBackend(this);
    wireBackend();
}

CloudSync::CloudSync(SyncBackend* backend, QObject* parent) : QObject(parent)
{
    backend_ = backend;
    backend_->setParent(this);   // adopt ownership so it dies with this CloudSync
    wireBackend();
}

void CloudSync::wireBackend()
{
    // Re-emit the backend's auth signals as CloudSync's own, so existing listeners (MainWindow, onboarding)
    // stay wired to CloudSync exactly as they were before the transport seam was extracted.
    connect(backend_, &SyncBackend::signedIn, this, &CloudSync::signedIn);
    connect(backend_, &SyncBackend::signInFailed, this, &CloudSync::signInFailed);
    connect(backend_, &SyncBackend::signedOut, this, &CloudSync::signedOut);
    // #27: the category relay is this account's — a sign-out, from wherever it comes, forgets it (and keeps what is
    // this device's own: #476, see forgetRelay).
    connect(backend_, &SyncBackend::signedOut, this, [] { forgetRelay(); });
}

// ---- static + auth forwarders to the Drive backend --------------------------------------------------
// The public API is unchanged; each of these is a one-line hand-off. The statics reach DriveSyncBackend's
// statics so callers like probe_cloudmerge's CloudSync::driveQueryQuote keep resolving.
bool CloudSync::isConfigured() { return DriveSyncBackend::isConfigured(); }
bool CloudSync::signInAvailable() { return DriveSyncBackend::signInAvailable(); }
QString CloudSync::driveQueryQuote(const QString& value) { return DriveSyncBackend::driveQueryQuote(value); }

bool CloudSync::isSignedIn() const { return backend_->isSignedIn(); }
QString CloudSync::accountEmail() const { return backend_->accountEmail(); }
PendingPush::Auth CloudSync::lastAuth() const { return backend_->lastAuth(); }
void CloudSync::signIn() { backend_->signIn(); }
void CloudSync::signOut() { backend_->signOut(); }

// ---- the six primitives — one-line forwarders to the backend ----------------------------------------
// These stay VIRTUAL and the orchestration below keeps calling them via `this`. In production `this` forwards
// to backend_ (the Drive transport); in the headless probes FakeCloud overrides them, substituting an in-memory
// Drive at exactly this seam and never touching backend_ — the behaviour that has always made the probes work.
void CloudSync::ensureFolder(std::function<void(const QString&)> cb)
{ backend_->ensureFolder(std::move(cb)); }
void CloudSync::findFolderNamed(const QString& name, std::function<void(bool, const QString&)> cb)
{ backend_->findFolderNamed(name, std::move(cb)); }
void CloudSync::renameFile(const QString& fileId, const QString& newName, std::function<void(bool)> cb)
{ backend_->renameFile(fileId, newName, std::move(cb)); }
void CloudSync::findFile(const QString& folderId, const QString& name,
                         std::function<void(bool, const QString&, const QString&, const QString&)> cb)
{ backend_->findFile(folderId, name, std::move(cb)); }
void CloudSync::uploadFile(const QString& folderId, const QString& existingId, const QString& name,
                           const QString& mimeType, const QByteArray& data, const QString& stateHash,
                           std::function<void(const QString&)> cb)
{ backend_->uploadFile(folderId, existingId, name, mimeType, data, stateHash, std::move(cb)); }
void CloudSync::downloadFile(const QString& fileId, std::function<void(bool, const QByteArray&)> cb)
{ backend_->downloadFile(fileId, std::move(cb)); }

void CloudSync::findBrandedFile(const QString& folderId, const QString& name, const QString& legacyName,
                                std::function<void(bool, const QString&, const QString&, const QString&)> cb)
{
    findFile(folderId, name, [this, folderId, legacyName, cb](bool listOk, const QString& id,
                                                              const QString& modIso, const QString& hash) {
        // Found it, or couldn't reach Drive at all, or the rename is already confirmed -> answer as-is.
        if (!listOk || !id.isEmpty() || BrandMigration::done(BrandMigration::Step::DriveFiles))
        { cb(listOk, id, modIso, hash); return; }
        findFile(folderId, legacyName, cb);   // an error here yields listOk=false, i.e. "unreachable", not "empty"
    });
}

// ---- state bundle (a zip of the synced settings + themes) --------------------------------------------

static const char* kBundleName = AppBrand::kSyncZip;

// ADD-ON CODE NEVER RIDES SYNC (issue #77, increment 2). The bundle used to zip addons/ minus the first-party
// folders, which meant third-party add-on code — registry-installed and sideloaded alike — was uploaded to a
// third party's disk and then unpacked and RUN on every other device on the account. None of it is in the
// bundle now, in either direction:
//   * a first-party add-on ships with the build (it always was excluded, so a stale cloud copy could not
//     clobber a freshly deployed one);
//   * a registry-installed add-on syncs as a REFERENCE in the merge document's roster section, and a receiving
//     device installs it from the registry itself (AddonRoster, AddonManager::applyMergedRoster);
//   * a sideloaded add-on is device-only, and the add-on list says so ("Not synced").
// An older peer still packs addons/ into its bundle; applyBundle refuses every such entry (see there).
static bool isAddonCodePath(const QString& name)
{
    return name.startsWith(QLatin1String("addons/")) || name == QLatin1String("addons");
}

static void appendSyncLog(const QString& msg)
{
    // stream_debug.log, the file every other sync diagnostic in this app lands in. Counts and a reason only.
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                 + QStringLiteral("\n")).toUtf8());
}

static void zipAddDir(mz_zip_archive& z, const QString& dir, const QString& prefix)
{
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        const QString rel = QDir(dir).relativeFilePath(path);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray data = f.readAll();
        const QString arch = prefix + QStringLiteral("/") + rel;
        mz_zip_writer_add_mem(&z, arch.toUtf8().constData(), data.constData(), data.size(), MZ_DEFAULT_COMPRESSION);
    }
}

// ---- the ONE device-local carve-out (mdsync T4) -----------------------------------------------------
// Applied in BOTH directions: buildBundle omits these from settings.json, applyBundle refuses to write them,
// and stateHash leaves them out of the sync fingerprint (so a device-local edit never reads as an unsynced
// change and never desyncs the cross-device baseline). Keep this the SINGLE source of truth for the list.
bool CloudSync::isDeviceLocalKey(const QString& key)
{
    // Exact device-local keys. Each has a SIBLING that DOES sync (profiles/list, sync/global/*,
    // library/showHidden) — so match the leaf exactly, never the group.
    static const QSet<QString> kExact = {
        QStringLiteral("roms/folder"),          // where THIS machine keeps its ROMs
        QStringLiteral("library/folder"),        // where THIS machine keeps its local video library
        QStringLiteral("interstitials/folder"),  // where THIS machine keeps its channel bumpers (#179 inc 2)
        QStringLiteral("emulators/root"),        // this machine's standalone-emulator install root
        QStringLiteral("emulators/fullscreen"),  // per-machine display preference
        QStringLiteral("player/externalPath"),   // this machine's external-player exe path
        QStringLiteral("player/external"),        // this machine's external-player choice
        QStringLiteral("netplay/relay"),          // this machine's relay endpoint
        QStringLiteral("display/mode"),           // TV / desktop / mobile form factor of THIS device
        QStringLiteral("display/tvPromptDone"),   // one-shot per-device onboarding flag
        QStringLiteral("onboarding/done"),        // first-run choice resolved on THIS device (must not sync back)
        QStringLiteral("profiles/current"),       // the active profile is per-device (profiles/list SYNCS)
        // RetroAchievements LOGIN (Achievements.cpp). ra/token is a session credential and ra/user its account
        // name — RA authentication is PER DEVICE (each machine logs in and holds its own token), so these must
        // never ride the synced settings bundle to another device. Matched as EXACT leaves, not an "ra/" prefix,
        // because ra/hardcore (issue #94) is a plain per-account PREFERENCE that DOES sync. (Pre-existing leak:
        // the token was uncarved until #94 touched this area.)
        QStringLiteral("ra/token"),
        QStringLiteral("ra/user"),
        // The OpenSubtitles LOGIN (#81): the username and password the first download asks for, and the rows in
        // Settings edit. A password must not ride a sync bundle, and #81 made this account a per-device thing:
        // any device can SEARCH with an API key alone, and each one asks for the login at its own first download.
        // Exact leaves, not a "subs/" prefix: the API key (subs/osApiKey) and every subtitle PREFERENCE under
        // subs/* still sync.
        QStringLiteral("subs/osUser"),
        QStringLiteral("subs/osPass"),
        // Trakt read-layer state (#23). Matched as EXACT leaves, never as a "trakt/" prefix, because
        // trakt/clientId and trakt/clientSecret are typed by the user and DO sync — set the app up
        // once, and it is set up everywhere.
        //
        // Two different reasons, both device-local:
        //   * The caches are a copy of data the other device can re-fetch in one request, and carrying
        //     them would flip the bundle's stateHash on every refresh — re-uploading the whole zip for
        //     a list nobody edited. Exactly the churn the per-item-store carve-out below exists to stop.
        //   * The BACKFILL WATERMARK is a claim about what THIS install has already imported. Synced,
        //     one device's completed run would suppress another device's first one, and the second
        //     device would report a complete import having written nothing. The MARKS the import
        //     produces sync normally through the progress document; the cursor that produced them
        //     must not.
        QStringLiteral("trakt/watchlistCache"),
        QStringLiteral("trakt/collectionCache"),
        QStringLiteral("trakt/watchlistCachedAt"),
        QStringLiteral("trakt/collectionCachedAt"),
        // The CALENDAR cache (#148). Same family, same first reason, but it is the one where the churn
        // argument stops being theoretical: TraktClient re-fetches the calendar on a 30-MINUTE cadence
        // and stamps trakt/calendarCachedAt with the wall clock on every complete run, whether or not a
        // single episode changed. Riding the bundle, that stamp alone flipped the fingerprint ~48 times
        // a day and re-uploaded the whole zip — addons, themes and settings — on a machine nobody
        // touched. #34 made it worse rather than merely wasteful: the retry machinery reads
        // localChanged as "a push is owed", so a cache timestamp kept presenting itself as a settings
        // change that had to be delivered.
        //
        // The payload is excluded for the ordinary reason: the other device re-fetches its own calendar
        // in one request, from ITS OWN Trakt account state, so a copy of ours is worth nothing there.
        //
        // These two were added to SettingsTxn::inScope's exclusion list by #23 and missed here, which is
        // why they are called out separately rather than folded into the list above.
        QStringLiteral("trakt/calendarCache"),
        QStringLiteral("trakt/calendarCachedAt"),
        // The flat backfill keys an earlier build of the #23 branch wrote, before the cursor was
        // namespaced per profile. Nothing writes them now and TraktClient removes them on disconnect;
        // they stay named here so an ini that still carries one cannot start syncing it in the
        // meantime. The LIVE cursor keys are matched by prefix below.
        QStringLiteral("trakt/listsCachedAt"),
        QStringLiteral("trakt/backfillThrough"),
        QStringLiteral("trakt/backfillDone"),
    };
    if (kExact.contains(key)) return true;
    // Prefix families that are wholly device-local.
    // Per-profile passcode ATTEMPT state (issue #30) — the failure count + lockout expiry. The passcode HASH
    // itself is inside profiles/list and SYNCS deliberately (set it once, it covers every device); the
    // lockout must NOT, because it is a property of who has been mashing the pad on THIS box. Syncing it
    // would let a kid's wrong guesses in the living room lock the parent out on the phone, and would push a
    // wall-clock deadline between devices whose clocks disagree.
    if (ProfilePasscode::isAttemptKey(key)) return true;
    // The backfill cursor, one entry PER PROFILE ("trakt/backfill/<profileId>/through" + "/done"), so a
    // list of exact keys would be one profile behind for ever. Matched through the prefix the pure layer
    // owns rather than a literal, so the two cannot drift; probe_cloudmerge pins that they agree.
    if (key.startsWith(trakt::backfillKeyPrefix())) return true;
    // MUSIC SCROBBLING (#192), both key families, and both matched through the pure layer's own prefixes so
    // the carve-out cannot drift from the writers. Two different reasons, and the first one is the reason this
    // carve-out exists at all:
    //
    //   * scrobble/<profile>/lb/token IS THE USER'S CREDENTIAL. A synced settings bundle is a zip in a Google
    //     Drive folder; a token in it is a token on a third party's disk, for a service the user linked on one
    //     machine. The secrets carve-out is deliberately excluded from sync for exactly this, and the on/off
    //     and custom URL ride with it because an enable that arrived on a device with no token would report
    //     itself as on while sending nothing.
    //   * scrobblestate/* is this DEVICE's accumulator: the delivered counter and the listens still waiting to
    //     be sent. Merging two devices' counters would report a number neither of them scrobbled, and merging
    //     two queues would submit the same listens twice — which is the double-count this feature is otherwise
    //     careful to avoid.
    if (Scrobble::isDeviceLocalKey(key)) return true;
    // "Play on device" (#143): the per-peer PAIRING TOKENS, matched through the pure layer's own predicate so
    // the carve-out cannot drift from the writer. A token is a credential minted BY another device FOR this
    // one -- it authorises /open on that peer, and it is meaningless anywhere else. Riding the synced bundle
    // it would be a credential in a zip in somebody's Drive folder (the reason the ListenBrainz token above
    // is carved out) AND it would hand every other install on the account the right to start playback on a
    // device it never paired with. Device-local in both directions, with no syncing sibling under the prefix.
    if (PlayOn::isDeviceLocalKey(key)) return true;
    // ANIME/MANGA TRACKERS (#156), both device-local families, matched through the pure layer's own
    // prefixes so the carve-out cannot drift from the writers. Two different reasons, and the first is
    // why the split with Trakt is deliberate rather than accidental:
    //
    //   * tracker/anilist/{clientId,clientSecret,access,refresh} ARE THE USER'S CREDENTIALS, and unlike
    //     trakt/clientId the TYPED PAIR is carved out here too. That is the issue's decision, not an
    //     oversight: an AniList client id and secret are a registered OAuth application belonging to one
    //     person, the synced bundle is a zip on a third party's disk, and "set it up once and it is set
    //     up everywhere" is not worth putting an OAuth client secret there. The user re-enters the pair
    //     on each device; the tokens were always per-device anyway (each machine completes its own
    //     authorization-code flow), exactly as ra/token and the Trakt tokens beside them are.
    //   * trackerstate/* is this DEVICE's accumulator: the undelivered progress queue and the per-item
    //     debounce stamps. Merging two devices' queues would submit the same chapter twice, and merging
    //     the stamps would suppress a push on the device that had not made it.
    //
    // The per-item LINKS are the inverse and are NOT here - they ride the merge document instead; see
    // isPerItemStoreKey below. probe_cloudmerge asserts both halves, because a later edit that moved the
    // links into this family would silently stop them syncing, and one that moved the credentials out of
    // it would silently start uploading a secret.
    if (tracker::isDeviceLocalKey(key)) return true;    // Discord presence (see Settings.h): whether THIS machine announces what it is playing. A shared TV
    // must not start broadcasting because presence was switched on for a laptop on the same account.
    if (key.startsWith(QLatin1String("discord/"))) return true;

    // openfail/* (issue #239): the last failed open, per item — "this press did nothing, and here is why",
    // written down so it outlives the toast that used to be its only trace. DEVICE-LOCAL because it is a fact
    // about THIS device's last attempt: this network, this source, this moment. The same title on the same
    // account may well open on the tablet in the next room, so syncing it would put one device's dead link on
    // another device's shelf, complete with a "Try again" that has nothing to retry — and it would linger
    // there for the full seven days, because the device that could clear it by succeeding is not the device
    // showing it. It is deliberately NOT in isPerItemStoreKey either: that family names the stores the
    // CloudMerge progress document owns, which is the other way a per-item key reaches a peer.
    if (key.startsWith(QLatin1String("openfail/"))) return true;
    // ADD-ON PASSWORD FIELDS (issue #77, increment 3): a setting an add-on declares `type: password` is stored at
    // addonsecret/<id>/<key>, with its one-shot migration stamp at addonsecretmig/<id>/<key>. The rest of an
    // add-on's settings (addoncfg/<id>/<key>) still sync, so an API key typed once is there everywhere; a
    // password is a credential for somebody's account, and the bundle is a zip on a third party's disk. The key
    // spellings live in AddonConfigKeys.h, shared with the writer, so this carve-out cannot drift from it.
    if (AddonConfigKeys::isDeviceLocal(key)) return true;
    return key.startsWith(QStringLiteral("emu/virtualPad")) // emu/virtualPad* (the on-screen pad, per device)
        || key.startsWith(QStringLiteral("sync/files/"))     // per-file A/V sync offsets (sync/global/* SYNCS)
        || key.startsWith(QStringLiteral("device/"))         // device/* (this install's identity — device/id)
        || key.startsWith(QStringLiteral("cloud/"))          // cloud/* (this device's OAuth tokens / client id)
        || key.startsWith(QStringLiteral("downloads"))       // downloads* (this device's local download catalog)
        // audio/* (issue #69): the output device, passthrough and exclusive mode. An audio-device id names a
        // sound card on THIS machine — syncing it would point device B at device A's output — and passthrough
        // / exclusive mode depend on what THIS box is wired to (an AV receiver, a bit-perfect DAC). The whole
        // group is per-device, so the prefix carves it out. There is no syncing "audio/" sibling: the shared
        // A/V-offset defaults live under "sync/global/*", not here. Contrast subs/* above, which DOES sync.
        || key.startsWith(QStringLiteral("audio/"))
        // launchhooks/* (issue #64): per-game pre-launch / post-exit command lines. Unlike #51's launchopts/*
        // (which SYNCS as a per-item store), a hook is a command line that EXECUTES — a synced one would run
        // on a different machine, where the path may not exist or the referenced tool isn't installed. Device-
        // local by design: it never rides the bundle and never enters the sync fingerprint.
        || key.startsWith(QStringLiteral("launchhooks/"))
        || key.startsWith(QStringLiteral("pcgames/"))        // pcgames/* (this device's installed PC games)
        // pcscan/* (issue #62): the persisted last-good installed-scan per launcher. It is a snapshot of
        // what THIS machine has installed on Steam/Epic/GOG/Battle.net — meaningful only here, and it churns
        // on every refresh — so it is device-local for the same reasons downloads/* and pcgames/* are, and
        // must never ride the synced settings bundle. probe_cloudmerge pins the carve-out.
        || key.startsWith(QStringLiteral("pcscan/"))
        // storebackend/* (issue #118): the store BACKENDS' cached owned-library listing and their signed-in
        // flag. Device-local for both of pcscan/*'s reasons and one of its own: the listing is a snapshot of
        // an account linked ON THIS MACHINE, in a third-party tool's own config, so it describes a sign-in
        // the receiving device does not have. Carried in a synced bundle it would present another machine's
        // Epic library as this one's, offering games nothing here can install. It also churns on the same
        // 30-minute cadence the Trakt caches do, which is the fingerprint-flipping cost that carve-out was
        // added for. probe_cloudmerge pins it.
        || key.startsWith(QStringLiteral("storebackend/"))
        // mediadur/* (issue #179): the measured length of each item this device has opened — the index a
        // channel's lineup is gated on. Device-local: it is re-derived by playing the file, it says nothing
        // about what anyone did, and left in the heavy settings bundle it would add a row per file ever opened
        // to the synced zip for a number the other device works out for itself the first time it plays
        // anything. MediaDurations keys everything under this prefix.
        || key.startsWith(QStringLiteral("mediadur/"))
        // previews/* (issue #85, carved out by #301): how much disk the seek-preview sprite sheets may
        // occupy on THIS machine. Not a credential and not churn - it is one number a user sets once - but
        // the wrong SHAPE for the bundle all the same, for emugfx's reason rather than downloads/'s: it
        // bounds a device-local thing. The sheets are generated here, live in this machine's cache directory
        // and are never shared, so the right bound is a function of this disk. A handheld with 32 GB and a
        // desktop with 4 TB do not want the same answer, and synced, setting it on one moved it on the
        // other. The whole prefix carves out cleanly: Settings keys only "previews/cacheMb" under it and
        // there is no syncing sibling to catch. probe_cloudmerge pins the classification.
        || key.startsWith(QStringLiteral("previews/"))
        // iptv/* (issue #75, increment 2): saved Live-TV playlist sources. The URL routinely embeds provider
        // credentials (…/get.php?username=X&password=Y), and it is not carved out anywhere else, so left in the
        // heavy settings bundle it would SILENTLY sync those credentials to every device. Device-local by
        // default: syncing credential-bearing source URLs is a deliberate opt-in for later (the store already
        // carries the change-hook), not something that ships by omission.
        || key.startsWith(QStringLiteral("iptv/"))
        // opds/* (issue #146): saved OPDS book catalogs. Each carries the self-hosted book server's URL (a LAN
        // address or private host — machine-specific) plus an optional HTTP basic-auth username/password. Left
        // in the heavy settings bundle it would SILENTLY sync those credentials to every device — exactly the
        // iptv footgun above. Device-local by default; a later opt-in could sync it, but it never ships by
        // omission. OpdsCatalogStore keys everything under this prefix; probe_cloudmerge pins the carve-out.
        || key.startsWith(QStringLiteral("opds/"))
        // subsonic/* (issue #193): saved Subsonic music servers. The same shape and the same hazard as the
        // two above, one notch worse — a Subsonic server is authenticated on EVERY request, so the stored
        // password is not optional the way an OPDS catalog's is. Left in the heavy settings bundle it would
        // put somebody's music-server password in a zip in a third party's Drive folder. Device-local, and
        // SubsonicServerStore keys everything under this prefix.
        || key.startsWith(QStringLiteral("subsonic/"))
        // jellyfin/* (issue #160): the connected Jellyfin servers. Same family as the three above, and the
        // credential is the strongest of the four — a Jellyfin ACCESS TOKEN is a bearer credential for a
        // whole account, usable from anywhere until it is revoked, and it sits beside a url that is often a
        // private LAN address meaningless on another machine anyway. Left in the heavy settings bundle it
        // would put that token in a zip in a third party's Drive folder. Device-local, and
        // JellyfinServerStore keys everything (server list, tokens, per-server enable) under this prefix.
        // probe_cloudmerge pins the carve-out.
        || key.startsWith(QStringLiteral("jellyfin/"))
        // audiobookshelf/* (issue #197): saved Audiobookshelf servers. Same shape and the same hazard as the
        // three above, with the credential in its most concentrated form — the stored value is an API TOKEN,
        // which is a standing grant against that server rather than something a login screen still stands
        // between. Left in the heavy settings bundle it would put that token in a zip in a third party's
        // Drive folder, for a server the user signed into on one machine. Device-local, and AbsServerStore
        // keys everything under this prefix. probe_cloudmerge pins the carve-out; probe_absclient byte-scans
        // a fixture token against everything the feature writes.
        || key.startsWith(QStringLiteral("audiobookshelf/"))
        // jellyseerr/* (issue #109): the request service's address and API KEY. The strongest credential in
        // this family and the one with the widest blast radius — a Jellyseerr key is a standing grant over a
        // household's whole acquisition pipeline, and it can queue downloads on somebody else's disk. Left
        // in the heavy settings bundle it would put that key in a zip in a third party's Drive folder. The
        // address rides with it for the same reason the OPDS and IPTV ones do: it is routinely a private LAN
        // name that means nothing on another machine, and it is half of what identifies the credential.
        // JellyseerrStore keys everything under this prefix; probe_requests byte-scans it.
        || key.startsWith(QStringLiteral("jellyseerr/"))
        // requests/* (issue #109): the rows on this profile's Requested shelf. NOT a credential — a row
        // names a title, its ids and a status token, and probe_requests asserts the key is absent from it —
        // but device-local for the followsnap/ and openfail/ reason: a row is a fact about the request
        // service THIS device is linked to. A peer with a different service, or none, would show rows it can
        // never refresh (complete with a "Ready to watch" badge for something not in its library), and the
        // device that could correct them is not the device showing them.
        || key.startsWith(QStringLiteral("requests/"))
        // followsnap/* (issue #155): what THIS device has already seen of each followed series, and which
        // children it has not shown you yet. The DEVICE-LOCAL half of the follow feature, and the inverse of
        // the "follow/" carve-out above. Same family and the same argument as #23's backfill watermark: it is
        // a claim about a fetch this install performed, so synced, one device's completed check would
        // suppress another device's first one and the second device would show an empty New shelf having
        // never asked anybody. A peer re-derives its own snapshot silently on its first check.
        || key.startsWith(QStringLiteral("followsnap/"))
        // emugfx* (issue #103): per-game/per-system standalone-emulator graphics (internal resolution / renderer
        // / …). Explicitly DEVICE-LOCAL — a 6x internal resolution a strong GPU eats will crawl on a weak one, so
        // syncing "run this game at 6x Vulkan" to every device is a footgun (EmuGfxStore.h says so). EmuGfxStore
        // uses two key spellings ("emugfx/items/…" and "\x01emugfx-system:…"); `contains` carves out both, and
        // no legitimate syncing key carries that token. (Left uncarved it would ride the heavy settings bundle.)
        || key.contains(QLatin1String("emugfx"))
        // shaderpreset* (issue #99): per-game/per-system slang-shader preset override. Explicitly DEVICE-LOCAL for
        // the SAME reason as emugfx — a shader's cost is hardware-dependent, so a Mega-Bezel chain a strong GPU
        // eats will crawl on a weak handheld; syncing "run this game under Mega-Bezel" to every device is a
        // footgun (ShaderPresetStore.h says so). ShaderPresetStore uses two key spellings ("shaderpreset/items/…"
        // and "\x01shaderpreset-system:…"); `contains` carves out both, and no legitimate syncing key carries
        // that token. (Left uncarved it would ride the heavy settings bundle.)
        || key.contains(QLatin1String("shaderpreset"));
}

// The per-item stores the progress merge document (CloudMerge) owns. applyBundle must never write these from
// the heavy bundle (release-gating: a peer's stale copy of stats/<this-device>/... would clobber the live
// accumulator namespace and then propagate on the next push).
//
// THE TABLE ITSELF LIVES IN core/PerItemStores.h (issue #332), with every entry's reason beside it, because
// SettingsTxn::inScope needs the same answer and a second hand-maintained copy of it drifted for years — see
// that header. A new per-item store is added THERE and this predicate inherits it.
bool CloudSync::isPerItemStoreKey(const QString& key)
{
    return peritem::isKey(key);
}

// ---- #27: what syncs, in the user's words, and this device's switches ---------------------------------------
// The carve-out is asked FIRST: several groups hold a device-local leaf beside syncing siblings (roms/folder vs
// roms/autoApplyPatches), and the table in SyncCategories.h names groups, not leaves.
synccat::Category CloudSync::categoryFor(const QString& key)
{
    if (isDeviceLocalKey(key)) return synccat::Category::DeviceLocal;
    return synccat::ofKey(key);
}

bool CloudSync::categoryEnabled(synccat::Category c)
{
    return synccat::isEnabled(store(), c);
}

bool CloudSync::keyTakenHere(const QString& key)
{
    if (isDeviceLocalKey(key)) return false;
    return synccat::isEnabled(store(), synccat::effective(synccat::ofKey(key)));
}

// A key the settings bundle carries at all, whatever the switches say: not a per-item store (the merge document
// owns those), not device-local, and not one of the roster's live or legacy keys (#77: the document's roster
// section is their authority). The per-item test goes FIRST: resume/ and marks/ rows are most of a big ini, and
// that table is two dozen prefixes where the carve-out is a long walk.
static bool isBundleSettingKey(const QString& k)
{
    return !CloudSync::isPerItemStoreKey(k) && !CloudSync::isDeviceLocalKey(k) && !AddonRoster::isLiveKey(k)
        && !AddonRoster::isLegacyRegistryKey(k);
}

// Whether `k`'s category is switched off here. Only asked once some switch is off, so a device on the defaults
// never walks the category table at all.
static bool offHere(const QString& k)
{
    return !synccat::isEnabled(store(), synccat::effective(synccat::ofKey(k)));
}

// THE SETTINGS THIS DEVICE UPLOADS — the one answer buildSettingsJson and the fingerprint both read, so the
// stamp a push carries and the hash this device compares against it can never describe two different bundles
// (#27 review, finding 2). It is this device's own value for every key of a category that is ON, plus, for a
// category switched OFF, the value the relay holds: the other devices' latest as of the last pull (or this
// device's own, frozen at the switch, until a pull has replaced them). A device that has just pulled therefore
// uploads exactly the bundle it pulled, whichever side has which category off, and the pull->push round keeps
// its fixed point. With every switch on — the default — this is byte-for-byte the set main always uploaded.
static QMap<QString, QString> uploadSettings()
{
    QMap<QString, QString> out;
    const bool anyOff = synccat::anySwitchedOff(store());
    for (const QString& k : store().allKeys())
    {
        if (!isBundleSettingKey(k)) continue;
        if (anyOff && offHere(k)) continue;
        out.insert(k, store().value(k).toString());
    }
    if (anyOff)
    {
        const QJsonObject carry = synccat::loadCarry(synccat::carrySettingsPart());
        for (auto it = carry.begin(); it != carry.end(); ++it)
            if (isBundleSettingKey(it.key()) && offHere(it.key())) out.insert(it.key(), it.value().toString());
    }
    return out;
}

bool CloudSync::setCategoryEnabled(synccat::Category c, bool on)
{
    if (!synccat::hasToggle(c)) return false;   // accounts, other settings and device-local have no switch
    if (categoryEnabled(c) == on) return true;
    QJsonObject all = synccat::loadCarryAll();
    QJsonObject relay = all.value(synccat::carrySettingsPart()).toObject();
    QJsonObject frozen = all.value(synccat::carryFrozenPart()).toObject();
    auto ofCat = [c](const QString& k) { return isBundleSettingKey(k) && synccat::effective(synccat::ofKey(k)) == c; };
    if (!on)
    {
        // Freeze: the relay starts as this device's own values, so the upload is unchanged by the switch (no
        // upload is owed for it, and the cloud copy does not lose the category while nobody has pulled yet). The
        // same values are kept as the SNAPSHOT a re-enable compares against.
        for (const QString& k : frozen.keys()) if (ofCat(k)) frozen.remove(k);
        for (const QString& k : relay.keys())  if (ofCat(k)) relay.remove(k);
        for (const QString& k : store().allKeys())
            if (ofCat(k)) { const QString v = store().value(k).toString(); relay.insert(k, v); frozen.insert(k, v); }
        all.insert(synccat::carrySettingsPart(), relay);
        all.insert(synccat::carryFrozenPart(), frozen);
        // No relay, no switch (the second review, finding 5): off with nothing relayed would drop the category
        // from the next upload. The caller tells the user; the category simply stays on.
        if (!synccat::saveCarryAll(all))
        {
            appendSyncLog(QStringLiteral("cloud sync: could not write the category relay - category %1 left on")
                              .arg(QLatin1String(synccat::id(c))));
            return false;
        }
        store().setValue(synccat::toggleKey(c), false);   // written only when OFF: ON is the absent default
        store().sync();
        return true;
    }
    // Back on: THIS category only — never a whole peer bundle (first review, finding 1) — and only where a peer
    // genuinely changed a key while the category was off: relay != this device's snapshot (second review,
    // finding 1). Everything else keeps the value this device holds NOW, including edits made while off (a comic
    // read on from page 50 to 200 stays at 200); such an edit simply reads as a local change and goes up with the
    // next push. Per key:
    //   in the relay, and different from the snapshot (or not in it)  -> a peer's change: adopted;
    //   in the relay, equal to the snapshot                            -> nobody else touched it: kept;
    //   in the snapshot only (the peer's bundle no longer carries it)  -> absence is not deletion: kept;
    //   in neither                                                     -> a key only this device has: kept.
    store().remove(synccat::toggleKey(c));
    bool committed = false;
    for (auto it = relay.begin(); it != relay.end(); ++it)
    {
        const QString& k = it.key();
        if (!ofCat(k)) continue;
        const QString v = it.value().toString();
        if (frozen.contains(k) && frozen.value(k).toString() == v) continue;   // the peer did not change it
        if (store().value(k).toString() == v) continue;                       // already there: no write
        // Written into the open settings visit, a later Discard would restore this device's stale values under a
        // switch that stays on (cloud/ is out of the transaction), and they would then be SENT. Close it first —
        // the same trade a remote apply makes (applySettingsJson).
        if (!committed && SettingsTxn::active()) { SettingsTxn::commit(); committed = true; }
        store().setValue(k, v);
    }
    for (const QString& k : frozen.keys()) if (ofCat(k)) frozen.remove(k);
    for (const QString& k : relay.keys())  if (ofCat(k)) relay.remove(k);
    all.insert(synccat::carrySettingsPart(), relay);
    all.insert(synccat::carryFrozenPart(), frozen);
    // A failed write here leaves stale entries for a category that is now ON: uploadSettings reads the relay only
    // for categories that are off, and the next freeze of this one replaces them, so it is logged and ignored.
    if (!synccat::saveCarryAll(all))
        appendSyncLog(QStringLiteral("cloud sync: could not tidy the category relay after switching %1 back on")
                          .arg(QLatin1String(synccat::id(c))));
    store().sync();
    return true;
}

// The relay's VALUES belong to one account: they are that account's, and uploading them into another would hand
// account Y whatever account X's devices last had. So they are forgotten on sign-out (wired in wireBackend, so
// every origin of a sign-out counts) and on a backend switch (MainWindow::switchSyncBackend).
//
// What belongs to this DEVICE is kept (#476). Forgetting the whole file used to drop the switch-off snapshot too,
// which cost twice over for a category still off: the first upload afterwards carried none of the category's keys
// or sections (the switch-off hole, reopened for a round), and the next re-enable, with no snapshot to compare
// against, adopted every relayed value over the edits made while off. So for each category still off, the relay
// goes back to exactly what the switch-off gave it:
//   settings  -> the snapshot ("frozen"), which only this device ever writes;
//   frozen    -> kept;
//   sections  -> this device's own serialisation of them, now (synccat::ownSwitchedOffSections, the same step
//                CloudMerge::switchCategory performs at a switch-off).
// Nothing that came from a pull survives it: every part is rebuilt from this device's own data, and the old file
// is replaced as a whole. A category that is on has nothing in the carry, so with every switch on the file simply
// goes, as it always did. If the rewrite fails, the file is removed instead: the one-round hole is the lesser harm.
void CloudSync::forgetRelay()
{
    QJsonObject frozen;
    if (synccat::anySwitchedOff(store()))
    {
        const QJsonObject was = synccat::loadCarry(synccat::carryFrozenPart());
        for (auto it = was.begin(); it != was.end(); ++it)
            if (isBundleSettingKey(it.key()) && offHere(it.key())) frozen.insert(it.key(), it.value());
    }
    QJsonObject own;
    own.insert(synccat::carrySettingsPart(), frozen);
    own.insert(synccat::carryFrozenPart(), frozen);
    if (const synccat::OwnSectionsFn sections = synccat::ownSwitchedOffSections())
        own.insert(synccat::carrySectionsPart(), sections());
    if (synccat::saveCarryAll(own)) return;
    appendSyncLog(QStringLiteral("cloud sync: could not reset the category relay to this device's own copy - removing it"));
    if (QFile::exists(synccat::carryPath()) && !QFile::remove(synccat::carryPath()))
        appendSyncLog(QStringLiteral("cloud sync: could not remove the category relay file"));
}

QByteArray CloudSync::buildSettingsJson()
{
    // Every setting except the device-local carve-out AND the per-item stores (mdsync T5 cadence fix). The
    // per-item stores are owned exclusively by the CloudMerge progress document, so they must NOT ride the
    // heavy bundle: applyBundle already refuses to write them inbound, and carrying them outbound only made a
    // per-item tick (a mark/favorite/playlist/stats accrual) flip the stateHash fingerprint and re-upload the
    // whole zip. Excluding them here (and in stateHash) keeps the heavy bundle quiet on per-item churn while the
    // lightweight merge doc still pushes on its own 15s debounce.
    //
    // The add-on roster's LIVE keys (addon.remote.urls / addon.enabled.*) are out too (issue #77): the merge
    // document's `roster` section is their one sync authority, and a last-writer-wins copy of them here is
    // exactly how a late push used to erase a subscription another device had just added. The registries the
    // user added (registry/addonsExtras, registry/themesExtras) are out for the same reason: they are roster
    // records now, and the old keys only exist on a device that has not yet adopted them.
    //
    // #27: exactly uploadSettings() — this device's values for the categories it has on, and the relayed values
    // for the ones it has off (see there). With every switch on it is the same set, and the same bytes, as ever.
    QJsonObject so;
    const QMap<QString, QString> up = uploadSettings();
    for (auto it = up.cbegin(); it != up.cend(); ++it) so.insert(it.key(), it.value());
    return QJsonDocument(so).toJson(QJsonDocument::Compact);
}

void CloudSync::applySettingsJson(const QByteArray& settingsJson)
{
    // A remote bundle writes settings-scope keys. If it lands while a settings transaction is open, a later
    // Discard would revert ANOTHER DEVICE's changes, not the user's — the snapshot predates this write, so
    // rollback() would see the peer's values as "changed" and put the local ones back. Commit the transaction
    // first: losing the ability to discard this visit is the correct trade against clobbering a peer.
    if (SettingsTxn::active()) SettingsTxn::commit();

    const QJsonObject so = QJsonDocument::fromJson(settingsJson).object();
    QString legacyUrls;                    // issue #77: an OLDER peer's roster snapshot, adopted below
    QHash<QString, bool> legacyFlags;
    bool legacyRoster = false;
    QVector<QPair<AddonRoster::RegistryList, QStringList>> legacyRegistries;   // #77: an older peer's added registries
    // #27: a category switched off here takes nothing from the peer. Its incoming values are RELAYED instead —
    // kept aside, never written to the ini — so this device's next upload carries the peer's view of it, not a
    // gap and not this device's own stale copy (see uploadSettings).
    const bool anyOff = synccat::anySwitchedOff(store());
    QJsonObject relay;
    for (auto it = so.begin(); it != so.end(); ++it)
    {
        const QString& k = it.key();
        // Inbound carve-out: never overwrite a device-local key, and never write a per-item store key (the
        // merge document owns those — writing them here would clobber this device's live/merged state).
        if (isDeviceLocalKey(k) || isPerItemStoreKey(k)) continue;
        if (anyOff && offHere(k))
        {
            if (!AddonRoster::isLiveKey(k) && !AddonRoster::isLegacyRegistryKey(k)) relay.insert(k, it.value().toString());
            continue;   // (an older peer's roster snapshot is the add-on category too: not adopted either)
        }
        // The roster's live keys are never written raw either (#77). Only a peer on a build before #77 still
        // sends them; its snapshot is kept aside and ADOPTED after the loop — so its manifest caches, which
        // do still ride the bundle, are already in place to key the entries by manifest id.
        if (AddonRoster::isLiveKey(k))
        {
            legacyRoster = true;
            if (k == QLatin1String("addon.remote.urls")) legacyUrls = it.value().toString();
            else legacyFlags.insert(k.mid(int(strlen("addon.enabled."))),
                                    QVariant(it.value().toString()).toBool());
            continue;
        }
        // An older peer's list of added registries: adopted as adds, never written raw over this device's roster.
        if (AddonRoster::isLegacyRegistryKey(k))
        {
            legacyRegistries.push_back({ k == QLatin1String("registry/addonsExtras") ? AddonRoster::RegistryList::Addons
                                                                                     : AddonRoster::RegistryList::Themes,
                                         AddonRoster::parseLegacyRegistryValue(it.value().toString()) });
            continue;
        }
        store().setValue(k, it.value().toString());
    }
    store().sync();
    // The relay is REPLACED, not merged: it is a copy of the peer's bundle, and the fixed point needs this
    // device's next upload to equal what it just pulled. It is never local data, so replacing it deletes nothing.
    // (An empty document is not a bundle — never let one wipe the relay.)
    if (anyOff && !so.isEmpty()) synccat::saveCarry(synccat::carrySettingsPart(), relay);
    // As ADDS, at ts 0, never over a local tombstone, never as a removal: the snapshot carries no dates, so the
    // union is the most it can honestly say. What it cannot say — that the old peer REMOVED something — waits
    // for that peer to upgrade and send a dated tombstone through the merge document.
    if (legacyRoster) AddonRoster::adoptLegacySnapshot(legacyUrls, legacyFlags);
    for (const auto& l : legacyRegistries) AddonRoster::adoptLegacyRegistrySources(l.first, l.second);
}

static QByteArray buildBundle()
{
    mz_zip_archive z; std::memset(&z, 0, sizeof(z));
    mz_zip_writer_init_heap(&z, 0, 0);

    const QJsonObject meta{ { QStringLiteral("device"), QSysInfo::machineHostName() },
                            { QStringLiteral("deviceId"), Settings::deviceId() }, // stable per-install id (mdsync T4)
                            { QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate) } };
    const QByteArray metaJson = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    mz_zip_writer_add_mem(&z, "meta.json", metaJson.constData(), metaJson.size(), MZ_DEFAULT_COMPRESSION);

    // All settings except the device-local carve-out (the ONE exclusion table).
    const QByteArray sJson = CloudSync::buildSettingsJson();
    mz_zip_writer_add_mem(&z, "settings.json", sJson.constData(), sJson.size(), MZ_DEFAULT_COMPRESSION);

    const QString app = AppPaths::dataDir();
    // No addons/ (issue #77): add-on code never rides sync. See isAddonCodePath above.
    zipAddDir(z, app + QStringLiteral("/themes"), QStringLiteral("themes"));
    // saves/ and states/ are NOT in the bundle: they sync per-file via SaveSync. They used to be here, which
    // meant (a) two devices silently overwrote each other's saves wholesale, and (b) every save write flipped
    // the fingerprint below and re-uploaded addons, themes and settings along with it.

    void* buf = nullptr; size_t sz = 0;
    mz_zip_writer_finalize_heap_archive(&z, &buf, &sz);
    QByteArray out(static_cast<const char*>(buf), int(sz));
    mz_zip_writer_end(&z);
    if (buf) mz_free(buf);
    return out;
}

static bool applyBundle(const QByteArray& data, int* refusedAddonFiles)
{
    if (refusedAddonFiles) *refusedAddonFiles = 0;
    mz_zip_archive z; std::memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_mem(&z, data.constData(), data.size(), 0)) return false;
    const QString app = AppPaths::dataDir();
    int refused = 0;
    const int n = int(mz_zip_reader_get_num_files(&z));
    for (int i = 0; i < n; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(&z, i)) continue;
        const QString name = QString::fromUtf8(st.m_filename);
        // SKIP (never delete) any saves/states an OLDER device is still packing into its bundle. SaveSync owns
        // these per-file now; extracting a legacy copy here would stomp a file the per-file pass just resolved
        // — and it would do so wholesale, which is exactly the loss this track removes. Deleting them locally
        // would be the other kind of wrong: this device's saves are live data, not bundle spillover. Checked
        // before the extract so a legacy bundle's saves aren't decompressed just to be thrown away.
        if (name.startsWith(QStringLiteral("saves/")) || name.startsWith(QStringLiteral("states/"))) continue;
        // REFUSE, never extract, add-on code from an older peer's bundle (issue #77). Writing it would install
        // and run code that arrived through sync, which is exactly what this build stopped doing: a registry
        // add-on reaches this device as a roster reference and is installed from its registry, and a sideloaded
        // one stays on the device it was sideloaded on. Counted and logged once below; checked before the
        // extract so the code is never even decompressed.
        if (isAddonCodePath(name)) { ++refused; continue; }
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
        if (!p) continue;
        const QByteArray bytes(static_cast<const char*>(p), int(sz));
        mz_free(p);

        if (name == QStringLiteral("settings.json"))
        {
            // Device-local keys AND per-item store keys are held off here (the merge document owns per-item).
            CloudSync::applySettingsJson(bytes);
        }
        else if (name.startsWith(QStringLiteral("themes/")))
        {
            // Restrict to the app dir (defend against path traversal in archive names).
            const QString dest = QDir::cleanPath(app + QStringLiteral("/") + name);
            if (!dest.startsWith(QDir::cleanPath(app) + QStringLiteral("/"))) continue;
            QDir().mkpath(QFileInfo(dest).absolutePath());
            QFile f(dest);
            if (f.open(QIODevice::WriteOnly)) { f.write(bytes); f.close(); }
        }
    }
    mz_zip_reader_end(&z);
    if (refused > 0)
        appendSyncLog(QStringLiteral("cloud sync: ignored %1 add-on file(s) in an older device's bundle - "
                                     "add-on code never rides sync").arg(refused));
    if (refusedAddonFiles) *refusedAddonFiles = refused;
    return true;
}

// A deterministic fingerprint of the local synced state (settings + theme files), independent of the
// zip's byte layout. Lets us tell "this device has unsynced edits" from "nothing changed".
static QByteArray stateHash()
{
    QCryptographicHash h(QCryptographicHash::Sha256);
    // Same carve-out as buildBundle: a device-local key isn't synced, so it must not enter the fingerprint —
    // otherwise a purely-local edit reads as an unsynced change and cross-device baselines never converge.
    // The per-item stores are ALSO excluded (mdsync T5): they're owned by the merge document, and if they
    // entered this fingerprint every mark/favorite/playlist/stats tick would read as "local changed" and
    // re-upload the heavy bundle. Keeping them out means per-item churn is served solely by the merge doc's
    // own push cadence; the bundle only re-uploads when a genuinely bundle-synced setting or file changes.
    // #77: the roster's live keys are the merge document's, so they are out here too. #27: the fingerprint hashes
    // EXACTLY the settings buildSettingsJson uploads (uploadSettings), in the same sorted key order and the same
    // k=v lines as always, so the stamp a push carries and this hash can never describe different bundles.
    const QMap<QString, QString> up = uploadSettings();
    for (auto it = up.cbegin(); it != up.cend(); ++it)
    { h.addData(it.key().toUtf8()); h.addData("="); h.addData(it.value().toUtf8()); h.addData("\n"); }

    const QString app = AppPaths::dataDir();
    // saves/ and states/ are NOT here either (save-sync T3), and this is the edit that pays for the track: a
    // per-file SHA of every save folded into this fingerprint is precisely WHY one F2 press read as "local
    // changed" and re-uploaded addons, themes and settings. SaveSync tracks those files' state itself.
    // Nor is addons/ (issue #77): it is not in the bundle, so installing or editing an add-on is not an
    // unsynced change, and must not re-upload the bundle.
    for (const QString& sub : { QStringLiteral("themes") })
    {
        const QString dir = app + QStringLiteral("/") + sub;
        QStringList files;
        QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) files << it.next();
        files.sort();
        for (const QString& f : files)
        {
            const QString rel = QDir(dir).relativeFilePath(f);
            QFile file(f);
            if (!file.open(QIODevice::ReadOnly)) continue;
            h.addData((sub + QStringLiteral("/") + rel).toUtf8());
            h.addData(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
        }
    }
    return h.result().toHex();
}

// Test seams (issue #77): the real bundle writer and reader, without the transport.
QByteArray CloudSync::buildStateBundle() { return buildBundle(); }
bool CloudSync::applyStateBundle(const QByteArray& zip, int* refusedAddonFiles)
{
    return applyBundle(zip, refusedAddonFiles);
}

// Test seam (mdsync T5): the same fingerprint the sync gate uses (checkStatus's st.localChanged). Exposed so
// the headless probe can assert per-item churn no longer flips it (i.e. the heavy bundle stays quiet).
QByteArray CloudSync::stateFingerprint() { return stateHash(); }

// checkStatus's localChanged, minus the network. ONE expression, shared by the status query and the push
// funnel, so "is anything actually owed?" cannot be answered two different ways in the same app.
bool CloudSync::localChangedSinceSync()
{
    return stateHash() != store().value(QStringLiteral("cloud/syncedHash")).toByteArray();
}

// The fixed point of the pull->push round, in one place. After this runs, localChangedSinceSync() is false, so
// the next checkStatus reports localChanged == false and PendingPush::resolve answers NothingToSend — which is
// what makes the round terminate rather than re-enter itself. Asserted in probe_cloudmerge §23.
void CloudSync::adoptSyncedBaseline(const QString& modifiedIso, const QString& remoteHash)
{
    store().setValue(QStringLiteral("cloud/appliedModified"), modifiedIso);
    // Baseline = the remote we just took, so a re-check sees neither side changed (no false conflict). A
    // legacy bundle carries no stamp, and then the state we have just applied is itself the baseline.
    store().setValue(QStringLiteral("cloud/syncedHash"),
                     remoteHash.isEmpty() ? stateHash() : remoteHash.toUtf8());
    store().sync();
}

void CloudSync::checkStatus(std::function<void(const Status&)> cb)
{
    ensureFolder([this, cb](const QString& folderId) {
        Status st;
        if (folderId.isEmpty()) { cb(st); return; } // unreachable
        st.reached = true;
        const QByteArray synced = store().value(QStringLiteral("cloud/syncedHash")).toByteArray();
        st.localChanged = localChangedSinceSync();   // the shared expression, so the push funnel cannot drift
        findBrandedFile(folderId, QString::fromLatin1(kBundleName), QString::fromLatin1(AppBrand::Legacy::kSyncZip),
                 [this, cb, st, synced](bool listOk, const QString& id, const QString& modIso, const QString& remoteHash) mutable {
            st.listReached = listOk;   // false => the file-query failed; "no bundle" is UNPROVEN, don't seed fresh
            st.hasRemote = !id.isEmpty();
            st.fileId = id;
            st.modifiedIso = modIso;
            st.remoteHash = remoteHash;
            // The remote differs from our last-synced baseline -> another device pushed. Compare content
            // hashes (robust); fall back to modifiedTime only for a legacy bundle without the hash stamp.
            if (!st.hasRemote)
                st.remoteChanged = false;
            else if (!remoteHash.isEmpty())
                st.remoteChanged = (remoteHash.toUtf8() != synced);
            else
                st.remoteChanged = (modIso != store().value(QStringLiteral("cloud/appliedModified")).toString());
            cb(st);
        });
    });
}

void CloudSync::applyRemote(const QString& fileId, const QString& modifiedIso, const QString& remoteHash,
                            std::function<void(bool)> cb)
{
    if (fileId.isEmpty()) { cb(false); return; }
    downloadFile(fileId, [modifiedIso, remoteHash, cb](bool ok, const QByteArray& data) {
        if (!ok || !applyBundle(data, nullptr)) { cb(false); return; }
        adoptSyncedBaseline(modifiedIso, remoteHash);
        cb(true);
    });
}

void CloudSync::pushLocal(std::function<void(bool, const QString&)> cb)
{
    ensureFolder([this, cb](const QString& folderId) {
        if (folderId.isEmpty()) { cb(false, tr("Couldn't reach Drive.")); return; }
        const QByteArray bundle = buildBundle();
        const QByteArray hash = stateHash();
        findBrandedFile(folderId, QString::fromLatin1(kBundleName), QString::fromLatin1(AppBrand::Legacy::kSyncZip), [this, folderId, bundle, hash, cb](bool listOk, const QString& id, const QString&, const QString&) {
            // A failed lookup returns an empty id; uploading with existingId="" would POST a DUPLICATE bundle instead
            // of PATCHing the real one. Bail so the caller retries rather than fragmenting the backup into two files.
            if (!listOk) { cb(false, tr("Couldn't reach Drive.")); return; }
            uploadFile(folderId, id, QString::fromLatin1(kBundleName), QStringLiteral("application/zip"), bundle,
                       QString::fromUtf8(hash),
                       [this, folderId, hash, cb](const QString& newId) {
                if (newId.isEmpty()) { cb(false, tr("Upload failed.")); return; }
                findBrandedFile(folderId, QString::fromLatin1(kBundleName), QString::fromLatin1(AppBrand::Legacy::kSyncZip), [this, hash, cb](bool, const QString&, const QString& modIso, const QString&) {
                    if (!modIso.isEmpty()) store().setValue(QStringLiteral("cloud/appliedModified"), modIso);
                    store().setValue(QStringLiteral("cloud/syncedHash"), hash);
                    store().sync();
                    cb(true, tr("Backed up to Google Drive."));
                });
            });
        });
    });
}

static const char* kProgressName = AppBrand::kProgressDoc;

void CloudSync::pullProgress(std::function<void(bool, const QByteArray&)> cb)
{
    ensureFolder([this, cb](const QString& folderId) {
        if (folderId.isEmpty()) { cb(false, QByteArray()); return; }
        findBrandedFile(folderId, QString::fromLatin1(kProgressName), QString::fromLatin1(AppBrand::Legacy::kProgressDoc), [this, cb](bool, const QString& id, const QString&, const QString&) {
            if (id.isEmpty()) { cb(true, QByteArray()); return; } // no progress file yet — a valid "nothing to merge"
            downloadFile(id, [cb](bool ok, const QByteArray& data) { cb(ok, data); });
        });
    });
}

void CloudSync::pushProgress(const QByteArray& json, std::function<void(bool)> cb)
{
    ensureFolder([this, json, cb](const QString& folderId) {
        if (folderId.isEmpty()) { cb(false); return; }
        findBrandedFile(folderId, QString::fromLatin1(kProgressName), QString::fromLatin1(AppBrand::Legacy::kProgressDoc), [this, folderId, json, cb](bool, const QString& id, const QString&, const QString&) {
            uploadFile(folderId, id, QString::fromLatin1(kProgressName), QStringLiteral("application/json"), json,
                       QString(), [cb](const QString& newId) { cb(!newId.isEmpty()); });
        });
    });
}
