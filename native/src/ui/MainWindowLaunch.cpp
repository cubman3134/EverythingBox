// LAUNCH PATHS (issue #186, increment 3b): MainWindow::remintAndOpen(), MainWindow::showRomhacks() and
// MainWindow::openLibraryItem(), in their own translation unit.
//
// WHY THEY MOVED. MainWindow.cpp is the build's single longest compile (a TU compiles on one core however
// many /MP gives the rest) and it already needs /bigobj. Increments 1 and 3a took the settings page and the
// panels out; these three are the next large, self-contained bodies, and the three that OPEN something: a
// Recents row re-minted from its source, a ROM's romhack page, and a library leaf.
//
// A PURE MOVE. Each function below is byte-for-byte what it was in MainWindow.cpp, leading comment included.
// So are the file-statics only they used, which moved with them unchanged: isPcPlatform(), fetchUrlBlocking(),
// RomhackBusyGuard and romhackPatchCachePath(). The helpers the rest of MainWindow.cpp also uses (mwLog(),
// logSafeUrl(), applyRemintRecipe() with its remintableId() guard, romLibraryFolderFor(),
// romhackPatchCacheDir() and describeTarget()) are defined once, in MainWindowInternal.h.
//
// NO GUARD. None of the three sat inside an #if region in MainWindow.cpp, so none does here.
#include "MainWindow.h"
#include "MainWindowInternal.h"   // mwLog(), logSafeUrl(), applyRemintRecipe(), the romhack paths: shared, defined once

#include "FeedbackPolicy.h"      // kFeedbackLong
#include "HomeView.h"
#include "MediaPane.h"
#include "Notifier.h"
#include "nav/NavOverlay.h"

#include "../addons/AddonManager.h"
#include "../comic/ChapterRun.h"
#include "../comic/ComicView.h"
#include "../core/AppPaths.h"
#include "../core/BattleNetLibrary.h"
#include "../core/UbisoftLibrary.h"
#include "../core/EaLibrary.h"
#include "../core/XboxLibrary.h"
#include "../core/BingeStore.h"
#include "../core/BoundedFetch.h"
#include "../core/CastManager.h"
#include "../core/CatalogMatch.h"
#include "../core/DownloadManager.h"
#include "../core/LocalLibrary.h"
#include "../core/PhotoLibrary.h"
#include "../core/Presence.h"
#include "../core/PresenceController.h"
#include "../core/ReadingForm.h"
#include "../core/RecentStore.h"
#include "../core/RomLibrary.h"
#include "../core/RomhackClient.h"
#include "../core/RomhackInstall.h"
#include "../core/Settings.h"
#include "../core/StreamHeaders.h"
#include "../core/SystemCatalog.h"
#include "../ebook/EbookFormats.h"
#include "../ebook/EbookView.h"
#include "../emu/RetroView.h"
#include "../media/PlaybackSession.h"
#include "../media/StreamResolver.h"
#include "../pdf/PdfView.h"
#include "../video/MpvWidget.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopedPointer>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>

// #224: ask a Recents row's SOURCE for a new link and open that, instead of replaying the dead one.
// Only reached from openRecent's url arm, for a row RecentStore::reopenFor sent to ResolveDirect or
// ResolveImdb — i.e. a row whose recipe is complete, so nothing here re-checks the routing.
void MainWindow::remintAndOpen(const RecentItem& row, const QString& resumeKey)
{
    if (!addons_) { notify(tr("Add-ons aren't ready yet — try again in a moment."), kFeedbackLong); return; }

    // SAY WHAT IS HAPPENING. A re-mint is a network round trip through the debrid provider — createtorrent,
    // a mylist poll, then requestdl — which on a cached release is a second or three and on a cold one is
    // longer. Silence there reads exactly like the freeze this issue is about. Sticky (ms <= 0) because a
    // step that can block for that long must not blink out halfway; every arm below ends it.
    notify(tr("Getting a fresh link for “%1”…").arg(row.title), 0);

    const QString title = row.title;
    const QString thumb = row.thumb;
    const QString kind  = row.kind;
    // The row's resume identity, NOT the url — which is the whole reason the position survives a re-mint.
    // Same expression playStream applies to its own argument (resumeKey-else-url), one layer earlier, so a
    // keyed row resumes on its key and a keyless one on the path HomeView handed us as the resume key.
    const QString rkey  = resumeKey.isEmpty() ? row.key : resumeKey;

    // STALENESS LATCH. A re-mint is a multi-second network round trip, and its callback used to fire
    // unconditionally: back out of this row, start something else, and the late answer OPENED THIS ROW OVER
    // WHAT THE USER CHOSE INSTEAD — with the sticky "Getting a fresh link…" toast sitting over the new item
    // until it did. Invisible in review, obvious in use. Same shape as nextEpGen_ (:5414) and remoteBookGen_
    // (:6579): capture by value now, compare in the callback, drop on a mismatch.
    //
    // TWO counters, for the two ways the user can move on, because neither covers the other's case:
    //   nextEpGen_  — an ORDINARY play started meanwhile. resetSegmentState() bumps it, and notePlaybackStart()
    //                 (the top of every play sink) calls that, as do the three setQueue routes that bypass the
    //                 hook. So any file/stream/game the user actually starts invalidates this answer.
    //   remintGen_  — ANOTHER RE-MINT started meanwhile, which is the likeliest sequel to backing out of a row
    //                 that is taking too long: pick the next Recents row, and it too resolves before it plays,
    //                 so it reaches no play sink and bumps nextEpGen_ not at all. Without this the first
    //                 answer would win the race and eat the second row's toast on its way past.
    // The third way out — backing out to Home and starting NOTHING — is covered by remintGen_ as well, but
    // from the other end: goBack() bumps it (see the block there for why bumping remintGen_ is safe where
    // bumping nextEpGen_ would break background audio). goBack() still bumps no PLAYBACK epoch, which is
    // deliberate and unchanged.
    const int    epGen  = nextEpGen_;
    const quint64 rmGen = ++remintGen_;
    // …and TAKE OWNERSHIP of the sticky notice just raised, so every arm below can ask "is the message on
    // screen still mine?" rather than the weaker "has a newer re-mint started?" (MainWindow.h, remintNoticeGen_).
    remintNoticeGen_ = rmGen;
    // Taking the channel means taking it from #217 too. The notify() above has already REPLACED any sticky
    // "that part wouldn't fetch" message on screen, so there is nothing to hide — but the record saying one is
    // up would survive it, and the next notePlaybackStart() (or playRemoteBookPart) would then hide OUR notice
    // believing it was the book's. Clearing the flag without calling hideNotice() is the whole fix: the state
    // is made to match the screen, and the message just raised stays raised.
    bookPartNoticeUp_ = false;

    auto onResolved = [this, title, thumb, kind, rkey, epGen, rmGen, row](const QString& url, const QString& mime,
                                                                         const StreamHeaders::Headers& headers)
    {
        Q_UNUSED(mime);
        if (epGen != nextEpGen_ || rmGen != remintGen_)
        {
            // Superseded: play NOTHING. The sticky toast above still has to come down, though — it has no
            // timeout, so a dropped callback that also dropped its notice would leave "Getting a fresh link…"
            // over the user's new choice for the rest of the session, which is half the bug. Only OUR OWN
            // notice: a newer re-mint, a newer #217 book-part message, or a goBack that already cleared it
            // each leave remintNoticeGen_ naming something that is not this call's to hide.
            if (remintNoticeGen_ == rmGen) { remintNoticeGen_ = 0; hideNotice(); }
            return;
        }
        if (url.isEmpty())
        {
            // The source could not mint one: the release is no longer on the account, or no longer cached.
            // Report it and NEVER take another release on the user's behalf. Silently substituting one drops
            // the viewer some way into a different cut with a resume position that refers to nothing, and
            // gives them nothing on screen explaining why. takeStreamNotice carries the source's own reason
            // when it had one ("caching started", "no seeds"), which is far better than anything invented here.
            //
            // AND IT DOES NOT NAME "Issue with Streaming", which it used to. That button is drawn over the
            // player, and this arm opened no player — the re-mint failed, so the user is still standing on
            // Home looking at the row they pressed. The message named a control that was not on screen and
            // could not be brought to it. The remedy that IS reachable from where they are is the item's own
            // shelf: opening it there resolves a fresh source (and, for a Stremio-resolved leaf, offers the
            // release picker beside Play). See MainWindow.h's armRemintSwap for why arming the swap from
            // this half-open state would be worse than saying so plainly.
            // A timed notify REPLACES the sticky one raised above, so this arm needs no separate hide — but
            // it does have to give the ownership record up, or a later hook would take THIS message down
            // early believing it was still the sticky "Getting a fresh link…" one.
            if (remintNoticeGen_ == rmGen) remintNoticeGen_ = 0;
            const QString why = addons_ ? addons_->takeStreamNotice() : QString();
            notify(why.isEmpty()
                       ? tr("Couldn't get a fresh link for “%1”. The release may no longer be on your debrid "
                            "account — open it from its shelf to try another source.").arg(title)
                       : tr("Couldn't get a fresh link for “%1”: %2").arg(title, why),
                   kFeedbackLong);
            return;
        }
        // Clear the sticky "Getting a fresh link…" toast. hideNotice(), not hidePlayerNotice(): those are two
        // different labels (Notifier.h — a window-level notice and a transient over the player), and the note
        // raised above is the window one. Hiding the wrong one would leave the toast up for the whole film.
        // Under the ownership record like every other arm — the latch above proves this re-mint is the live
        // one, but not that its message is still the one being displayed.
        if (remintNoticeGen_ == rmGen) { remintNoticeGen_ = 0; hideNotice(); }
        // The FRESH headers ride the callback, exactly as StreamCb's contract requires (AddonManager.h:28:
        // a member holding "the last stream's headers" outlives its stream and sends host A's Referer to
        // host B). They are used here and never written down — #59 is untouched by this change.
        //
        // BOTH ARMS PASS THEM EXPLICITLY. openStreamUrl/openAudioStream default `headers` to empty precisely
        // so a caller with nothing to give CLEARS the previous stream's headers rather than inheriting them
        // (#59) — so leaving the default here would throw away the headers we just resolved and a gated
        // source would 403 on the one path built to fix it.
        // CARRY THE RECIPE BACK INTO THE ROW THIS RE-OPEN REWRITES — on BOTH arms, because both sinks write
        // a KEYED Recents entry and RecentStore::add adopts a prior row's recipe only for a keyless one. A
        // re-mint that passed nothing would blank the very recipe that made this re-mint possible, and #224
        // would work exactly once per row: the first re-open succeeds, then reopenFor sees no recipe and
        // sends the row back to replaying a link #200 stripped the credential from. The item is
        // reconstituted from the ROW rather than from the resolve, because the row IS the recipe:
        // applyRemintRecipe reads back precisely the fields it wrote, so the rewrite is a fixed point and
        // the tenth re-open carries the same four fields as the first.
        MediaItem played;
        played.sourceAddonId = row.sourceAddonId;
        if (row.sourceRoute == QLatin1String("imdb")) played.imdbStreamId = row.sourceItemId;
        else { played.id = row.sourceItemId; played.type = row.sourceType; }
        // Name and artwork, which the recipe does not carry and the ROW does. applyRemintRecipe ignores both
        // (it reads four fields and no more), so they are here purely for the source swap armed below: that
        // swap re-opens this item through the ordinary catalog sink, which files its Recents entry under the
        // item's own title and cover. Without them a swap would rename the user's Continue Watching row to
        // whatever file name the new CDN link ends in.
        played.title = title;
        played.thumbnailUrl = thumb;
        // …and its RESUME IDENTITY on the imdb leg. A direct row's key IS its sourceItemId (the video leaf
        // records rkey = item.id, and applyRemintRecipe copies that same id into the recipe), so `played.id`
        // is already right there. An imdb row's recipe stores the imdbStreamId, which is NOT always the id
        // the row is keyed by — a bridged catalog item is filed under "tmdb:123" while its stream recipe
        // says "tt0111161" — and a swap that opened under the wrong key would resume at 0:00 and leave a
        // duplicate row behind. applyRemintRecipe's imdb branch returns before it reads `id`, so filling it
        // in here cannot disturb the recipe this same item is about to write.
        if (row.sourceRoute == QLatin1String("imdb")) played.id = rkey;
        // ONLY IF THE OPEN ACTUALLY REACHED A PLAY SINK. Three of the shapes below return from the open
        // without playing anything, and arming over any of them puts a "try another source" button on the
        // chrome of something else:
        //   an .m3u8/.m3u link — openStreamUrl hands it to streams_->resolve and RETURNS. Whatever plays,
        //     plays later and through a different signal (playDirect, or playQueue for a channel list), so
        //     the arm would sit over a queue the user is now steering.
        //   an external player — playStream hands the link to VLC and returns, having cleared the flag.
        //   a split pane      — the stream opens in the pane; the main window's chrome is not its chrome.
        // `nextEpGen_` is the test because it is the one thing every real sink does and none of the three
        // early returns does: notePlaybackStart() is the top of each sink and bumps it through
        // resetSegmentState(). Cheaper and harder to desync than re-deriving isM3uRef/routePlay's decision
        // here — routePlay in particular CONSUMES a one-shot override, so it cannot honestly be asked twice.
        const int sinkGen = nextEpGen_;
        if (kind == QStringLiteral("audio")) openAudioStream(url, rkey, title, thumb, headers, &played);
        else                                 openStreamUrl(url, rkey, title, headers, &played);
        if (sinkGen == nextEpGen_) return;   // nothing opened here: leave the swap unarmed
        // AFTER the open, never before: playStream clears currentNextSourceCapable_ (a pasted or replayed
        // link is not swappable, which is still the honest default for every other caller) and reveals the
        // chrome while it is false. Arming here and re-revealing is what puts the button on screen for this
        // stream instead of at the next mouse move.
        armRemintSwap(played, row.sourceRoute, row.sourceType);
    };

    if (row.sourceRoute == QLatin1String("imdb"))
    {
        // No addon named: this fans out across every installed stream provider, which is why reopenFor lets
        // an imdb row through regardless of whether the addon that originally served it is still here.
        //
        // THE RELEASE THE VIEWER IS ACTUALLY WATCHING, not merely the best one available now. Without this
        // the re-mint takes the current top candidate, and the resume offset stored against this row — the
        // entire point of #224 — lands in a DIFFERENT RIP: seconds to minutes out, with the quality and the
        // audio track quietly changed underneath them. applyRemintRecipe writes sourceItemId = the item's
        // imdbStreamId on this route, so it is the same "ttShow:S:E" shape the next-episode hand-off passes
        // at :5419, and one lookup definition serves both. Empty for a movie (seriesKeyFor answers empty for
        // any id without an S:E tail) and for a series never played from the picker, which is exactly the
        // no-memory default the parameter already documents: take the best candidate.
        addons_->resolveStreamByImdb(row.sourceType, row.sourceItemId, onResolved, /*attempt=*/0,
                                     BingeStore::preferredGroup(bingeStore_.get(), row.sourceItemId));
        return;
    }
    // "direct": ask the one addon that knows this id space. reopenFor already refused the row when
    // sourceById returned null (SourceMissing), so this pointer is the one openRecent just tested.
    LoadedAddon* src = addons_->sourceById(row.sourceAddonId);
    MediaItem item;
    item.id    = row.sourceItemId;
    item.type  = row.sourceType;
    item.title = row.title;

    // AN AUDIOBOOK IS LISTED, NOT RESOLVED TO ONE LINK — and this is the one place the distinction could
    // have been lost. resolveStream hands back whichever single file the provider returns first, which is
    // #214's original defect (a fifteen-hour book opening at part 10) walked back in through the re-mint
    // door: the row would re-open, play something, and be wrong in a way that reads as the app working.
    //
    // resolveAudiobookRelease re-lists the release and signs PART ONE ONLY. That is not a shortcut but the
    // invariant its own header states (AddonManager.h): signing forty links here would be one round trip
    // instead of forty, and thirty-nine would have expired before the listener reached them — a queue that
    // dies an hour in is a worse failure than the one being fixed, and it looks like the app breaking rather
    // than like a link ageing out. Every later part mints from its id when the app REACHES it, which is what
    // openRemoteAudiobook's token queue exists to make possible.
    //
    // ROUTED ON sourceType, NOT ON row.kind. Kind is "audio" for a book AND for a remote music track — the
    // two share a Recents kind deliberately, because they share a re-open route — but a track is one file and
    // has no release to expand, so sending it through the /detail expansion would ask a question its id
    // cannot answer. sourceType is the item's own type, written by applyRemintRecipe from the item that
    // played, and it is "audiobook" for exactly the rows this branch is for.
    //
    // THE OTHER THREE TESTS RESTATE THE BROWSE GATE, WHICH IS SPELT ACROSS TWO NESTED `if`s THERE AND ONE
    // FLAT CONDITION HERE — read either half of it alone and this looks like a divergence it is not.
    // HomeView's only call to resolveAudiobookRelease (HomeView.cpp:7297) sits inside
    //     :7270  if (addon && addon->transport == LoadedAddon::RemoteHttp)   <- non-null AND the transport
    //     :7281      if (fileProvider && it.type == "audiobook")             <- fileProvider = !addon->stremio
    // so `fileProvider` on its own carries NO transport test (:7272) and the whole gate has all three. The
    // condition below is that conjunction written out, and it has to be: a Stremio addon's /detail is a
    // series' episodes with no release to expand, and re-minting a row by a route that never opened it is
    // the divergence LeafRoute.h exists to end.
    //
    // The one asymmetry is what supplies the type. Browse tests the LIVE item's `type`; this tests the
    // RECORDED sourceType — the same value only because applyRemintRecipe copies item.type into it, so the
    // two gates agree by construction of the recipe rather than by coincidence.
    //
    // A null `src` falls through for a related but separate reason: resolveStream refuses it politely on its
    // first line (AddonManager.cpp:2433) while resolveAudiobookRelease dereferences it on its first line.
    if (row.sourceType == QLatin1String("audiobook")
        && src && !src->stremio && src->transport == LoadedAddon::RemoteHttp)
    {
        item.thumbnailUrl = thumb;   // the now-playing cover, and the artwork the rewritten row keeps (#201)
        // The addon that owns this id space, carried ON THE ITEM because applyRemintRecipe reads it from
        // there: without it the row this re-open rewrites would come back recipe-less and the next re-open
        // would replay a dead link again. Set only on this branch — resolveStream ignores the field, but the
        // video route has no reason to grow a line it does not use.
        item.sourceAddonId = row.sourceAddonId;
        addons_->resolveAudiobookRelease(src, item, [this, item, title, thumb, rkey, epGen, rmGen](
                                                        const AddonManager::DocFind& found) {
            // THE SAME STALENESS LATCH the stream callback above carries, and this arm needs it more: a book
            // re-mint is TWO round trips (the /detail expansion, then part one's link), so it is the slowest
            // answer this function can give and the likeliest to land after the user has moved on. Without
            // it, backing out of a slow book and picking something else would be answered by the book
            // opening over that choice — with a fifteen-hour queue behind it.
            if (epGen != nextEpGen_ || rmGen != remintGen_)
            {
                // Only OUR OWN notice comes down — see the stream callback above and remintNoticeGen_'s
                // contract in MainWindow.h.
                if (remintNoticeGen_ == rmGen) { remintNoticeGen_ = 0; hideNotice(); }
                return;
            }
            if (found.url.isEmpty())
            {
                // The provider's own words first — the precedence the browse path established. noAudio and
                // noPartLink are causes somebody ESTABLISHED; "the release may no longer be on your account"
                // is a guess, and #216 is the record of what asserting the wrong guess costs (it sent a user
                // to look at their debrid account while the app held a list of 57 playable files).
                // A timed notify REPLACES the sticky one raised above, so this arm needs no separate hide —
                // but it gives the ownership record up, as the stream callback's empty-url arm does.
                //
                // "Issue with Streaming" is not named here either, and for TWO reasons rather than the
                // stream arm's one: this failure opened no player to host the button, AND a book never
                // offers that verb even when one is playing (openRemoteAudiobook turns it off — swapping
                // part three for another release's part three is not a thing the user could mean).
                if (remintNoticeGen_ == rmGen) remintNoticeGen_ = 0;
                notify(!found.notice.isEmpty()
                           ? tr("Couldn't get a fresh link for “%1”: %2").arg(title, found.notice)
                       : found.noAudio
                           ? tr("“%1” has no audio files in it any more — the release it came from may have "
                                "changed. Open it from its shelf to try another source.").arg(title)
                       : found.noPartLink
                           ? tr("“%1” was found and its parts were listed, but no link for the first part "
                                "came back. Try again in a moment.").arg(title)
                           : tr("Couldn't get a fresh link for “%1”. The release may no longer be on your "
                                "debrid account — open it from its shelf to try another source.").arg(title),
                       kFeedbackLong);
                return;
            }
            // The window-level notice raised above, not the player's — they are two labels — and only while
            // it is still ours to hide.
            if (remintNoticeGen_ == rmGen) { remintNoticeGen_ = 0; hideNotice(); }
            MediaItem m = item;
            m.url = found.url; m.mime = found.mime; m.bookParts = found.parts;
            // THE BOOK IS KEYED BY THE ROW'S KEY, AND RE-MINTED BY THE RELEASE ID — two identities that are
            // the same string on one route and different strings on the other, which is why they are now
            // written apart. `item.id` above is sourceItemId, the id the RESOLVE needed; the queue below is
            // keyed by `m.id`, and keying it by the resolve id would rename every part token of a book found
            // through the doc-bridge (where the row is filed under a catalog id like "googlebooks:…"). That
            // failure is the silent kind the queue's own header warns about: a fifteen-hour book resuming at
            // 0:00 of part one with no error anywhere. For a row played off the provider's own shelf the two
            // are the same value and this assignment changes nothing.
            m.id = rkey;
            // …and the recipe rides back so the REWRITTEN row can be re-minted again. Without this pair the
            // row would come back naming its catalog id (which resolves nothing), and #224 would work exactly
            // once per book — the first re-open succeeds, the second is dead. Same fixed-point rule the
            // stream arm's `played` follows, spelt with the override fields because on this route the mint
            // identity is not the item's own.
            // (Read off `item`, which IS the recipe: its sourceAddonId and id were filled from
            // row.sourceAddonId / row.sourceItemId above, and the copy into `m` happened before the line
            // that re-keys it. No extra capture, and nothing to keep in step with the row.)
            m.remintAddonId = item.sourceAddonId;
            m.remintItemId  = item.id;
            // THE QUEUE IS REBUILT, not merely its first link, and that is what makes the resume position
            // survive. openRemoteAudiobook keys every part token on bookKey + fileName, and bookKey is
            // `item.id.isEmpty() ? item.title : item.id` — here item.id is row.sourceItemId, which is the
            // very id applyRemintRecipe copied out of the item that wrote the row, and which reopenFor
            // refuses to route on when empty. So the key is the same string it was on the first play, every
            // token matches, and the listener lands back in the part they left. Were it NOT the same string
            // the failure would be silent: a queue of tokens nothing has a position for, resuming at 0:00 of
            // part one with no error to show for it.
            if (m.bookParts.size() > 1) { openRemoteAudiobook(m, found.url); return; }
            // One part (or a release that no longer expands): a single recording, played and re-recorded
            // with its recipe intact so the NEXT re-open still has one. No headers — this resolver carries
            // none back, exactly as the browse path it mirrors carries none forward.
            //
            // NO SOURCE SWAP IS ARMED ON EITHER BOOK ARM, and this one-part shape is the tempting exception.
            // A swap re-resolves through resolveStream, which hands back ONE ARBITRARY FILE of whatever
            // release answers next — #214's original defect (a fifteen-hour book opening at part 10). This
            // arm cannot know that the alternate release is also one part, so offering the verb here would
            // offer it for books in general, which openRemoteAudiobook already refuses for the reason stated
            // at its own currentNextSourceCapable_ line: a book is not one item.
            openAudioStream(found.url, rkey, title, thumb, {}, &m);
        });
        return;
    }
    addons_->resolveStream(src, item, onResolved);
}

// A game whose platform is desktop Windows isn't an emulator ROM — we run it on the PC itself. The addon
// tags these with the platform name it was opened from (e.g. "PC (Windows)").
static bool isPcPlatform(const QString& hint)
{
    const QString h = hint.trimmed().toLower();
    return h == QStringLiteral("pc (windows)") || h == QStringLiteral("pc (microsoft windows)")
        || h == QStringLiteral("pc windows") || h == QStringLiteral("windows") || h == QStringLiteral("pc");
}

// One blocking GET with a deadline, for the romhack flow's two JSON waits. The nav kit's pick/ask are already
// blocking, so an async chain here would buy nothing but a state machine; the deadline is what keeps a dead
// server from looking like a hung app. Any failure is an empty body, which every caller reads as "nothing".
//
// JSON waits only, now. The third caller was the PATCH, and a patch is the one response in this flow whose
// size is set by the file rather than by a reply — so it went to BoundedFetch, which can abandon a response
// too large to buffer. Buffering the whole body is safe here precisely because a listing is a listing.
static QByteArray fetchUrlBlocking(const QString& url, int timeoutMs)
{
    QNetworkAccessManager nam;
    QNetworkRequest rq{ QUrl(url) };
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QScopedPointer<QNetworkReply> reply(nam.get(rq));

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply.data(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    deadline.start(timeoutMs);
    loop.exec();

    if (!reply->isFinished()) { reply->abort(); return QByteArray(); }
    if (reply->error() != QNetworkReply::NoError) return QByteArray();
    return reply->readAll();
}

// Clears the in-flight flag on every way out of the romhack flow — and there are many: a backed-out menu,
// an empty result, a failed fetch, a declined install. A flag cleared by hand at each return is a flag that
// gets missed on the next one added, and the miss leaves the verb dead until a restart.
namespace {
struct RomhackBusyGuard
{
    explicit RomhackBusyGuard(bool* flag) : flag_(flag) {}
    ~RomhackBusyGuard() { if (flag_) *flag_ = false; }
    RomhackBusyGuard(const RomhackBusyGuard&) = delete;
    RomhackBusyGuard& operator=(const RomhackBusyGuard&) = delete;
    bool* flag_;
};
}

// A stable, path-safe name for one hack's one patch file. Hashed rather than sanitised: a source's file name
// is not ours to trust as a path component, and any sanitisation broad enough to make it safe is also broad
// enough to map two different patches onto one name — which would hand back the wrong file, silently, to
// someone who had asked for the second. Stable is the load-bearing property: it is what makes enqueue()'s
// de-dup-by-destination resume an interrupted transfer instead of starting a second one.
static QString romhackPatchCachePath(const QString& hackId, const QString& patchName)
{
    const QByteArray key = hackId.toUtf8() + '\0' + patchName.toUtf8();
    return romhackPatchCacheDir() + QLatin1Char('/')
         + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex())
         + QStringLiteral(".patch");
}

// ---- Romhacks (retro game leaves) -----------------------------------------------------------------------
// One synchronous flow, deliberately: every step needs the previous answer, and the nav kit's pick/ask are
// blocking by design. Each network wait shows a busy note so a slow source never looks like a dead button.
void MainWindow::showRomhacks(const MediaItem& item, const QString& systemId)
{
    const QStringList servers = addons_ ? addons_->remoteSourceUrls() : QStringList();
    if (servers.isEmpty())
    {
        notify(tr("Romhacks come from your server — add one in Settings first."), 5000);
        return;
    }

    // Every wait below runs a nested event loop with no overlay covering the UI, so the app stays live and
    // the same leaf can be activated again mid-flow. One flow at a time: two would interleave their status
    // notes and could race each other's install of the same base ROM.
    //
    // The refusal says so rather than dropping the press silently. It used to hold for a moment; it now spans
    // a patch download with a three-minute deadline, so a quiet refusal is three minutes of pressing the verb
    // and watching the app do nothing — indistinguishable, from the outside, from a dead button.
    //
    // Over-sticky and not plain notify(): the flow it refuses is parked on a STICKY phase note in this same
    // single label, so a plain toast would overwrite that note and then hide the label three seconds later —
    // the second press would blank the wait it was complaining about, which is worse than the dead button.
    if (romhackBusy_)
    {
        if (notifier_) notifier_->notifyOverSticky(tr("Already fetching a romhack — one at a time."), 3000);
        return;
    }
    romhackBusy_ = true;
    const RomhackBusyGuard busyGuard(&romhackBusy_);

    // Here rather than at startup: this is the only feature that writes to that folder, so it is the only
    // place that has any business sweeping it, and it costs one directory listing on a path someone is
    // already waiting on the network for.
    pruneRomhackPatchCache();

    const QString title = item.title.trimmed();
    notify(tr("Looking for romhacks for %1…").arg(title), 0);   // sticky: a phase note must not blink out

    // Ask every configured server and merge. A server that is down or has no romhack source contributes
    // nothing and never stops the others being offered.
    QVector<RomhackEntry> hacks;
    QHash<QString, QString> serverForId;
    for (const QString& base : servers)
    {
        const QByteArray body = fetchUrlBlocking(RomhackClient::listUrl(base, systemId, title), 20000);
        for (const RomhackEntry& e : RomhackClient::parseList(body))
        {
            if (serverForId.contains(e.id)) continue;   // the same hack from two servers is still one hack
            serverForId.insert(e.id, base);
            hacks.push_back(e);
        }
    }

    if (hacks.isEmpty())
    {
        notify(tr("No romhacks found for %1.").arg(title), 4000);
        return;
    }

    // Split hacks from translations. They are different things wanted for different reasons — one changes a
    // game you can already read, the other makes a game readable at all — and a popular title can carry
    // hundreds of the first, which buries the handful of the second beyond any amount of scrolling. Grouped
    // only when both exist: a single-group chooser would be a menu whose every path leads to the same place.
    QVector<RomhackEntry> translations, others;
    for (const RomhackEntry& e : hacks)
    {
        if (e.category.compare(QStringLiteral("Translation"), Qt::CaseInsensitive) == 0) translations << e;
        else others << e;
    }

    RomhackEntry chosen;
    bool picked = false;
    while (!picked)
    {
        QVector<RomhackEntry> group = hacks;
        QString heading = tr("Romhacks for %1").arg(title);
        if (!translations.isEmpty() && !others.isEmpty())
        {
            const QStringList groups = { tr("🧩  Hacks (%1)").arg(others.size()),
                                         tr("🗪  Translations (%1)").arg(translations.size()) };
            const int which = NavMenu::pick(tr("Romhacks for %1").arg(title), groups, this);
            if (which < 0) { hideNotice(); return; }        // backed out of the whole thing
            group = (which == 1) ? translations : others;
            heading = (which == 1) ? tr("Translations for %1").arg(title) : tr("Hacks for %1").arg(title);
        }

        QStringList rows;
        rows.reserve(group.size());
        for (const RomhackEntry& e : group) rows << e.menuLabel();
        const int pick = NavMenu::pick(heading, rows, this);
        if (pick >= 0 && pick < group.size()) { chosen = group[pick]; picked = true; break; }

        // Backing out of a group returns to the group chooser rather than abandoning the flow — the whole
        // point of splitting was to let someone look in one list and then the other.
        if (translations.isEmpty() || others.isEmpty()) { hideNotice(); return; }
    }

    notify(tr("Fetching %1…").arg(chosen.title), 0);
    const QByteArray body = fetchUrlBlocking(
        RomhackClient::fetchUrl(serverForId.value(chosen.id), chosen.id), 60000);
    const RomhackFetch fetched = RomhackClient::parseFetch(body);
    if (!fetched.valid)
    {
        notify(tr("Couldn't get that hack's patch."), 5000);
        return;
    }

    // A release shipping a patch per ROM revision must be asked about, never coin-flipped: picking the
    // wrong one produces a game that looks fine and breaks later.
    int which = 0;
    if (fetched.patches.size() > 1)
    {
        QStringList names;
        for (const RomhackPatchFile& p : fetched.patches) names << p.name;
        which = NavMenu::pick(tr("This hack ships %n patch(es) — which one?", "", int(fetched.patches.size())),
                              names, this);
        if (which < 0) { hideNotice(); return; }
    }
    const RomhackPatchFile& patch = fetched.patches[which];

    PendingRomhack req;
    req.base = item;
    req.systemId = systemId;
    req.hack = chosen;
    req.target = fetched.target;

    // Some hacks are published as the FINISHED GAME rather than as a patch. Then there is nothing to apply:
    // no base ROM to find, no dump to match, and nothing to warn anyone about — so none of the rest of this
    // function happens. It is also the one route on which a translation cannot go wrong (see below).
    if (patch.format == QStringLiteral("rom"))
    {
        const QString targetDir = romLibraryFolderFor(systemId);
        if (targetDir.isEmpty())
        {
            notify(tr("Set your ROMs folder in Settings before installing a hack."), 7000);
            return;
        }
        // Named for itself, not "Base (Hack)": the file already carries both, and doubling them would read
        // "Arkanoid (Arkanoid (J) [T-Port])".
        //
        // …except when the release ships MORE THAN ONE, in which case the picked file's own base name goes
        // in too. The chooser above exists because the revisions differ, but the hack TITLE is one string for
        // all of them — so on the title alone every revision names the same path, and the short-circuit just
        // below would then see the FIRST one already sitting there, download nothing, write this hack's
        // metadata over it and say it installed the second. Silent, and with the two files indistinguishable
        // afterwards. The base name is passed rather than pre-composed into the title because the title is
        // length-capped when it is sanitised, and a long hack name would swallow the part that distinguishes
        // them; it also keeps `chosen.title` the name shown and stored, which is still the hack's.
        const QString variant = fetched.patches.size() > 1 ? QFileInfo(patch.name).completeBaseName()
                                                           : QString();
        const QString dest = RomhackInstall::destinationForRom(
            chosen.title, QFileInfo(patch.name).suffix(), targetDir, variant);
        if (dest.isEmpty())
        {
            notify(tr("That hack's name can't be used as a file name."), 8000);
            return;
        }

        // Already there? Then it is installed, and we adopt it instead of fetching it again. This is a
        // DELIBERATE CHANGE from the byte-carrying path this replaced, which removed the destination and
        // renamed over it — a re-install always overwrote. Re-downloading a disc image to reproduce a file
        // already on disk costs hours and gains nothing, and someone re-running the install is asking for
        // the game to be there, not for those bytes to be fetched a second time. The cost is that a
        // corrupt or truncated existing file can no longer be repaired by re-installing: it has to be
        // deleted first. Answered HERE rather than by letting enqueue() notice, because enqueue() reports an
        // existing file by emitting jobCompleted SYNCHRONOUSLY — and what that handler reaches opens
        // NavConfirm, which must never run inside another emission (#28).
        if (QFileInfo::exists(dest) && QFileInfo(dest).size() > 0)
        {
            finishRomhackInstall(dest, chosen.title, req);
            return;
        }

        // Already fetching this exact hack? Then say so and stop. The guard that keeps the rest of this
        // function single-flight is released the moment it RETURNS, which on this route is right after
        // enqueue() — so the download itself is unguarded, and nothing downstream catches the repeat: only
        // the ".part" exists yet, so the check above passes, and enqueue() de-dups by destination while the
        // handler below matches on key, folding a second press into ONE job carrying TWO handlers. Both
        // would fire, the second inside the first's NavConfirm loop, which is the #28 shape.
        //
        // Refused only while the manager STILL HOLDS that job: cancelling from the Downloads panel drops the
        // job without ever emitting jobCompleted, so the id would otherwise sit in the set until the process
        // ended and a perfectly reasonable retry would be told a lie. Letting the retry through re-arms a
        // second handler beside the cancelled one's (which nothing can disconnect), so the handler honours
        // only the FIRST completion per id — see there.
        DownloadJob::State heldState = DownloadJob::Done;
        const bool jobStillHeld = dm_ && [&] {
            for (const DownloadJob& j : dm_->jobs()) if (j.dest == dest) { heldState = j.state; return true; }
            return false;
        }();
        if (romhackRomDownloads_.contains(chosen.id) && jobStillHeld)
        {
            // The refusal is the same either way; only the sentence changes. "Still held" is not the same as
            // "still moving": finishActive leaves a FAILED job in the list (a 404 does not remove it) and a
            // job the user paused stays too — only removeJob/clearFinished drop either. Telling someone their
            // hack "is already downloading" when the transfer died an hour ago sends them to watch a progress
            // bar that will never advance, so a stopped job says it is stopped and names the one place the
            // Retry and the Resume actually live.
            const bool stopped = heldState == DownloadJob::Failed || heldState == DownloadJob::Paused;
            notify(stopped
                       ? tr("%1 stopped downloading — resume or retry it in Downloads.").arg(chosen.title)
                       : tr("%1 is already downloading — it's in Downloads.").arg(chosen.title),
                   6000);
            return;
        }

        const QString downloadUrl = RomhackClient::fileUrl(serverForId.value(chosen.id), patch.url);
        if (downloadUrl.isEmpty() || !dm_)
        {
            notify(tr("Couldn't work out where to download %1 from.").arg(chosen.title), 8000);
            return;
        }

        // Straight into the ROMs folder through the ORDINARY download path — one queue, one progress UI, one
        // place to cancel — because a finished hack IS a game, and this is how games arrive. The manager
        // streams to a sibling ".part" and renames, which is the write discipline the byte-carrying path
        // used to perform by hand, and it resumes: at disc size a dropped connection is not a rare event.
        DownloadJob job;
        job.title = chosen.title;
        job.url = downloadUrl;
        job.dest = dest;
        job.kind = QStringLiteral("game");
        job.sysId = systemId;
        job.thumb = item.thumbnailUrl;
        job.key = chosen.id;      // the only handle we get back; the id is minted inside the manager

        // A one-shot owned by the connection itself rather than by a member slot. Unlike the base-ROM wait
        // there is no second callback that has to disarm this one, and a member would make two hacks queued
        // together exclusive for no reason.
        //
        // KNOWN GAP — this handler does not survive a restart, and neither does the transfer under it. The
        // JOB is persisted, but save() writes an Active job out as Paused and the manager's constructor
        // demotes any Active job to Paused on load, while pump() starts only QUEUED ones — so a download
        // interrupted by quitting the app comes back stopped and waits for a manual Resume. Closing mid-
        // transfer (an ordinary case at disc size, not an edge one) therefore leaves only the ".part": no
        // playable file, no metadata, no rescan, no "Play it now?".
        //
        // Recovering it is still ONE step, from the hack's page, because the three things that have to line
        // up all do. The in-flight set above is memory-only, so after a restart it is empty and the guard
        // lets the re-run through. enqueue() de-dups by destination and flips a Paused or Failed job back to
        // Queued, then pumps — so the re-run RESUMES the old transfer off its ".part" rather than starting a
        // second one. And `key` IS persisted, so this freshly-armed handler matches the restored job when it
        // finishes and does the metadata write, the rescan and the prompt exactly as it would have. Nothing
        // asks the user to find the Downloads panel first.
        romhackRomDownloads_.insert(chosen.id);
        auto* conn = new QMetaObject::Connection;
        const QString wantKey = chosen.id;
        const PendingRomhack pending = req;      // by value: this outlives the frame that built it
        *conn = connect(dm_, &DownloadManager::jobCompleted, this,
                        [this, conn, wantKey, pending](const DownloadJob& done) {
            if (done.key != wantKey) return;
            disconnect(*conn);
            delete conn;
            // Disarmed here, above the branch, so EVERY way out of this handler clears it — the file landed
            // or it didn't, either way nothing is in flight for this hack any more and asking again must be
            // allowed. (A repeat after a success meets the destination-exists short-circuit instead.)
            //
            // Its absence is also how a completion gets honoured EXACTLY ONCE. A cancel-then-retry leaves an
            // older handler armed on the same key with no way to disconnect it, so two can see the same
            // finish; the first to run takes the id, and the rest find it gone and stand down. Two that both
            // ran would stack a second confirm inside the first's nested loop — the #28 shape.
            if (!romhackRomDownloads_.remove(wantKey)) return;
            if (done.dest.isEmpty() || !QFileInfo::exists(done.dest))
            {
                notify(tr("%1 didn't download, so it wasn't installed.").arg(pending.hack.title), 8000);
                return;
            }
            // Off this frame before anything opens a nested loop: jobCompleted can be emitted from inside
            // enqueue(), and finishRomhackInstall ends in NavConfirm. See #28.
            const QString landed = done.dest;
            deferPastQmlEmission([this, landed, pending] {
                finishRomhackInstall(landed, pending.hack.title, pending);
            });
        });

        // Bounded, NOT sticky. A sticky notice here is only ever cleared by hideNotice() or the next notify(),
        // and cancelling the job from the Downloads panel goes through dm_->cancel(), which emits no
        // jobCompleted — so the overlay went on announcing a download that had been stopped. This line only
        // has to say the transfer started; the Downloads panel holds the progress and the Cancel, and that is
        // where anyone watching it should be looking.
        notify(tr("Downloading %1…").arg(chosen.title), 8000);
        // This job carries no proxy headers, so NetHeaderApply leaves it on Qt's NoLessSafeRedirectPolicy and
        // it WILL follow a cross-host 302 — isSafeRelativeFileUrl only guarantees where the transfer STARTS.
        // That is a decision, not an oversight: the request carries no headers, no cookies and no credentials,
        // so a redirect leaks nothing, and a server sitting behind a reverse proxy or a CDN needs the hop
        // followed or its files are simply unreachable.
        dm_->enqueue(job);
        return;
    }

    // Asked BEFORE anything is downloaded or unpacked, because it is a question about the PATCH and needs no
    // ROM to answer. Asking it after would mean interrupting someone at the end of a long download to tell
    // them the thing they already chose might not fit.
    // The source publishes no checksum and no target dump name, so for IPS there is nothing to verify
    // against and this confirm is the only gate. BPS and UPS carry their own source CRC32 and are refused
    // by the applier below regardless of what is answered here.
    const bool selfChecking = patch.format == QStringLiteral("bps") || patch.format == QStringLiteral("ups");
    // The source stated a dump we can actually check the ROM against. Then there is nothing to warn about: a
    // wrong copy is REFUSED below rather than risked, so asking someone to accept a risk we are about to
    // eliminate would be theatre — and worse, it would train them to click through the warning that matters.
    const bool verifiable = fetched.target.checkable();
    const bool translation = chosen.category.compare(QStringLiteral("Translation"), Qt::CaseInsensitive) == 0;
    if (!verifiable && (!selfChecking || translation))
    {
        QString msg;
        if (!selfChecking)
            msg = tr("This patch can't be checked against your copy of %1 — its format carries no "
                     "checksum, and the source doesn't say which dump it was built for.").arg(title);
        // A translation is built against the release that needed translating — normally the Japanese one —
        // and NOT against the English release most libraries hold. That is the usual reason a translation
        // patch produces a broken game, and no format check catches it: an IPS applies to any bytes at all.
        if (translation)
        {
            if (!msg.isEmpty()) msg += QStringLiteral("\n\n");
            msg += tr("This is a translation, so it patches the release that was translated FROM — usually "
                      "the Japanese one. Applied to an English copy of %1 it will not produce a working "
                      "game.").arg(title);
        }
        if (!fetched.targetNote.trimmed().isEmpty())
            msg += tr("\n\nWhat the author said:\n%1").arg(fetched.targetNote.trimmed().left(600));
        msg += tr("\n\nYour game stays untouched either way — the hack installs as a separate copy.");
        if (NavConfirm::ask(tr("Install %1?").arg(chosen.title), msg,
                            { tr("Cancel"), tr("Install") }, /*focusIndex*/ 0, /*cancelIndex*/ 0, this) != 1)
        {
            // Declining ends the flow, so the "Fetching …" phase note has to go with it. Sticky notes are
            // cleared by hideNotice() or by the next notify() and by nothing else, so a bare return left the
            // wait on screen for the rest of the session — every other decline on this route already does
            // this. It matters more since notifyOverSticky: a transient posted over this note restores it
            // when it expires, so a stranded phase note comes BACK after being covered.
            hideNotice();
            return;
        }
    }

    // A hack can be browsed and chosen for a game that is not downloaded yet — the list is keyed by title and
    // console and needs no ROM. Only the APPLY needs one, so a missing base game is an extra step rather than
    // a dead end: fetch the game, then patch it.
    //
    // Asked BEFORE the patch is fetched, for the same reason the warning above is: it is a question about the
    // ROM and needs no patch to answer. Asking it afterwards means interrupting someone minutes after a
    // disc-scale patch finished downloading, to ask about something that was knowable before it started —
    // and on the queued route the confirm would arrive with the flow long since off screen. From here on
    // there are no more questions, only transfers.
    const QString baseRom = item.url;
    const bool needBaseRom = baseRom.isEmpty() || !QFileInfo::exists(baseRom);
    if (needBaseRom)
    {
        hideNotice();
        QString msg = tr("You don't have %1 yet, and a hack is a patch for it — so both are needed.\n\n"
                         "%1 downloads to your library as an ordinary game, and %2 installs beside it "
                         "as a separate copy.").arg(title, chosen.title);
        // Say WHICH release it needs, while there is still a decision to make. A translation is normally
        // built against the Japanese dump, and what downloads here is whatever your sources offer for the
        // title — so knowing the target now is the difference between a working install and a refusal after
        // the download has already run.
        const QString wanted = describeTarget(req.target);
        if (!wanted.isEmpty())
            msg += tr("\n\n%1 was built for %2. If the copy that downloads isn't that release it won't be "
                      "installed, and nothing will be written to your library.").arg(chosen.title, wanted);
        if (NavConfirm::ask(tr("Download %1 first?").arg(title), msg,
                            { tr("Cancel"), tr("Download both") }, /*focusIndex*/ 1, /*cancelIndex*/ 0, this) != 1)
            return;
    }

    // Acquire the patch, and let the RESPONSE decide how. Usually small — a disc-scale RELEASE arrives as a
    // finished ROM and left through the download queue above — but a patch BUILT AGAINST a disc image is
    // itself disc-scale, and nothing here can tell the two apart in advance: the format is asserted by the
    // source and never sniffed, so a two-byte tweak and a rebuild of a whole disc arrive under one extension.
    //
    // So the fetch judges itself. Under the ceiling it finishes here, on this frame, off the queue and out of
    // the Downloaded folder — which is what the queue could not offer, running one job at a time and
    // recording everything it finishes. Over the ceiling it is abandoned at the cost of one response head and
    // handed to the manager, which streams to a ".part", resumes with a Range request, and shows progress and
    // a Cancel. The one thing that was never acceptable is what used to happen: a disc-scale transfer with
    // none of that, behind a deadline it could not meet.
    //
    // BoundedFetch leaves Qt on NoLessSafeRedirectPolicy, so this request WILL follow a cross-host 302 —
    // RomhackClient::fileUrl only guarantees where the transfer STARTS, not where it ends. That is a decision
    // and not an oversight: the request carries no headers, no cookies and no credentials, so a redirect leaks
    // nothing, and a server behind a reverse proxy or a CDN needs the hop followed or its files are simply
    // unreachable. An unexamined default and a considered one look identical in code, so it is written here.
    const QString patchUrl = RomhackClient::fileUrl(serverForId.value(chosen.id), patch.url);
    // A reference we will not follow, or no server to resolve it against. Nearly unreachable — parseFetch
    // already drops a patch whose url is not a safe relative reference — but it is guarded HERE because the
    // alternative is silent and wrong: an empty url produces a request that never reaches a host, which
    // arrives back as a failure with no status, which the branch below words as "the source didn't answer in
    // time". That sends someone to wait and retry over a reference that will never work. The finished-ROM
    // route makes the same check for the same reason.
    if (patchUrl.isEmpty())
    {
        notify(tr("Couldn't work out where to download %1's patch from.").arg(chosen.title), 8000);
        return;
    }
    req.patchPath = romhackPatchCachePath(chosen.id, patch.name);
    QDir().mkpath(romhackPatchCacheDir());

    // Already here? Then it was fetched before and not yet consumed — an install that failed, or one whose
    // patch download outlived the app. Use it, and touch nothing. Answered HERE rather than by letting
    // enqueue() notice, because enqueue() reports an existing file by emitting jobCompleted SYNCHRONOUSLY,
    // and what that handler reaches opens NavConfirm — which must never run inside another emission (#28).
    if (QFileInfo::exists(req.patchPath) && QFileInfo(req.patchPath).size() > 0)
    {
        resumeRomhackAfterPatch(req, needBaseRom);
        return;
    }

    // Names the PATCH, where the note before the chooser named the hack. They are two network operations with
    // two deadlines, and one unchanging sentence across both means a stall in the second reads as a stall in
    // the first.
    notify(tr("Fetching %1's patch…").arg(chosen.title), 0);
    // Sixty seconds, not the three minutes this used to take. The deadline no longer has to cover a
    // disc-scale transfer, because a disc-scale transfer no longer happens here — so it can be sized for what
    // does happen, and "couldn't download it, try again" becomes true for the first time.
    static constexpr qint64 kPatchInlineCeiling = 16 * 1024 * 1024;
    const BoundedFetch::Result fetchedPatch = BoundedFetch::get(patchUrl, 60000, kPatchInlineCeiling);

    if (fetchedPatch.verdict == BoundedFetch::Result::Failed)
    {
        mwLog(QStringLiteral("romhack: patch fetch failed for \"%1\" — status %2, %3, from %4")
                  .arg(chosen.title).arg(fetchedPatch.status)
                  .arg(fetchedPatch.error.isEmpty() ? QStringLiteral("-") : fetchedPatch.error,
                       logSafeUrl(patchUrl)));
        // Two different things to do next, so two different sentences. A server that ANSWERED and refused is
        // most often one whose fetched file has aged out of its timed store — the chooser was left open too
        // long — and the fix is to ask for the hack again, not to press the same dead reference. A server
        // that never answered is a source or a network problem, and retrying is exactly right. The old single
        // sentence sent everyone down the second road, including everyone for whom it led nowhere.
        notify(fetchedPatch.status >= 400
                   ? tr("%1's patch isn't on the server any more — open the hack again to refresh it.")
                         .arg(chosen.title)
                   : tr("Couldn't download %1's patch — the source didn't answer in time.").arg(chosen.title),
               8000);
        return;
    }

    if (fetchedPatch.verdict == BoundedFetch::Result::Ok)
    {
        // Onto our own disk before anything else can happen to it: the apply can be an hour away, behind a
        // base-ROM download.
        QFile pf(req.patchPath);
        if (!pf.open(QIODevice::WriteOnly) || pf.write(fetchedPatch.body) != fetchedPatch.body.size())
        {
            notify(tr("Couldn't save %1's patch — check there's space for it.").arg(chosen.title), 8000);
            return;
        }
        pf.close();
        resumeRomhackAfterPatch(req, needBaseRom);
        return;
    }

    // Over the ceiling. Through the ORDINARY download path from here — one queue, one progress UI, one place
    // to cancel — because at this size that is what the transfer needs, and it is what the finished-ROM route
    // above already does for the same reason.
    if (!dm_)
    {
        notify(tr("Couldn't download %1's patch.").arg(chosen.title), 8000);
        return;
    }

    // Already fetching this exact hack's patch? Say so and stop. Refused only while the manager STILL HOLDS
    // the job: cancelling from the Downloads panel drops it without ever emitting jobCompleted, so the id
    // would otherwise sit in the set until the process ended and a perfectly reasonable retry would be told a
    // lie. Letting the retry through re-arms a second handler beside the cancelled one's, which nothing can
    // disconnect — so the handler below honours only the FIRST completion per id.
    DownloadJob::State heldState = DownloadJob::Done;
    const QString patchDest = req.patchPath;
    const bool patchJobHeld = [&] {
        for (const DownloadJob& j : dm_->jobs()) if (j.dest == patchDest) { heldState = j.state; return true; }
        return false;
    }();
    if (romhackPatchDownloads_.contains(chosen.id) && patchJobHeld)
    {
        // "Still held" is not "still moving": a failed job stays in the list, and so does a paused one. Being
        // told a patch "is already downloading" when its transfer died an hour ago sends someone to watch a
        // bar that will never advance, so a stopped job says it is stopped and names where the Retry lives.
        const bool stopped = heldState == DownloadJob::Failed || heldState == DownloadJob::Paused;
        notify(stopped
                   ? tr("%1's patch stopped downloading — resume or retry it in Downloads.").arg(chosen.title)
                   : tr("%1's patch is already downloading — it's in Downloads.").arg(chosen.title),
               6000);
        return;
    }

    DownloadJob patchJob;
    // Reads as an intermediate, not as the game. Someone scanning Downloads must not conclude the hack has
    // already arrived — it has not; this is the step before it.
    patchJob.title = tr("%1 (patch)").arg(chosen.title);
    patchJob.url = patchUrl;
    patchJob.dest = req.patchPath;
    patchJob.kind = QStringLiteral("patch");
    patchJob.thumb = item.thumbnailUrl;   // the row is otherwise blank, and the art says which install this is
    patchJob.key = chosen.id;             // the only handle back; the job id is minted inside the manager
    patchJob.record = false;              // an intermediate: the Downloads panel, and nowhere else

    // A one-shot owned by the connection itself rather than by a member slot — a member would make two hacks
    // queued together exclusive for no reason.
    romhackPatchDownloads_.insert(chosen.id);
    auto* patchConn = new QMetaObject::Connection;
    const QString wantPatchKey = chosen.id;
    const PendingRomhack pendingPatch = req;   // by value: this outlives the frame that built it — and it is
                                               // a path now, so the copy costs nothing
    const bool wantBaseRom = needBaseRom;
    const QString patchHackTitle = chosen.title;
    *patchConn = connect(dm_, &DownloadManager::jobCompleted, this,
                         [this, patchConn, wantPatchKey, pendingPatch, wantBaseRom, patchHackTitle]
                         (const DownloadJob& done) {
        if (done.key != wantPatchKey) return;
        disconnect(*patchConn);
        delete patchConn;
        // Disarmed above the branch, so EVERY way out clears it. Its absence is also how a completion is
        // honoured EXACTLY ONCE: a cancel-then-retry leaves an older handler armed on the same key with no
        // way to disconnect it, so two can see the same finish. The first takes the id; the rest stand down.
        // Two that both ran would stack a second confirm inside the first's nested loop — the #28 shape.
        if (!romhackPatchDownloads_.remove(wantPatchKey)) return;
        if (done.dest.isEmpty() || !QFileInfo::exists(done.dest))
        {
            notify(tr("%1's patch didn't download, so it wasn't installed.").arg(patchHackTitle), 8000);
            return;
        }
        // Off this frame before anything opens a nested loop: jobCompleted is emitted from inside
        // finishActive, which still holds a live reference into its own jobs_ vector and has not yet cleared
        // activeId_, saved, or pumped — and this continuation ends in NavConfirm. See #28. Nothing index-like
        // survives the hop: `pendingPatch` is already a copy and carries a plain path.
        deferPastQmlEmission([this, pendingPatch, wantBaseRom] {
            resumeRomhackAfterPatch(pendingPatch, wantBaseRom);
        });
    });

    // Bounded, NOT sticky. A sticky notice here is only ever cleared by hideNotice() or the next notify(),
    // and cancelling the job from the Downloads panel goes through dm_->cancel(), which emits no
    // jobCompleted — so the overlay would go on announcing a download that had been stopped. This line only
    // has to say the transfer started; the panel holds the progress and the Cancel.
    notify(tr("Downloading %1's patch…").arg(chosen.title), 8000);
    dm_->enqueue(patchJob);
}

void MainWindow::launchUbisoftGame(const QString& key, const QString& uri, const QString& title,
                                   const QString& thumb)
{
    const QString id = UbisoftLibrary::idFrom(key, uri);
    const QString url = UbisoftLibrary::launchUri(id);   // rebuilt from the id: a recorded URI is never replayed raw
    if (url.isEmpty())
    {
        statusBar()->showMessage(tr("No playable file is associated with “%1” yet.").arg(title), kFeedbackLong);
        return;
    }
    const QString recentKey = QStringLiteral("ubi:") + id;
    // handOffStoreLaunch is the one openUrl for a store URI. Under EB_UITEST_STORE_URI_SINK it logs the URI as
    // held back instead of opening it, which is how a UI-test drive proves this dispatch without a Ubisoft
    // client. (No play-time watch: LaunchWatch knows steam:// and the Epic URI only, so this stays a plain
    // fire-and-forget handoff.)
    handOffStoreLaunch(url, recentKey);
    RecentStore::add({ url, title, QStringLiteral("ubisoftgame"), thumb, recentKey });
    statusBar()->showMessage(tr("Launching “%1” via Ubisoft Connect…").arg(title), 5000);
}

void MainWindow::launchEaGame(const QString& key, const QString& uri, const QString& title, const QString& thumb)
{
    const QString id = EaLibrary::idFrom(key, uri);
    const QString url = EaLibrary::launchUri(id);   // rebuilt from the id: a recorded URI is never replayed raw
    if (url.isEmpty())
    {
        statusBar()->showMessage(tr("No playable file is associated with “%1” yet.").arg(title), kFeedbackLong);
        return;
    }
    const QString recentKey = QStringLiteral("ea:") + id;
    // The same one store-URI openUrl as Ubisoft: under EB_UITEST_STORE_URI_SINK it logs the URI as held back
    // instead of opening it, which is how a UI-test drive proves this dispatch without the EA app. (No
    // play-time watch: LaunchWatch knows steam:// and the Epic URI only, so this is a fire-and-forget handoff.)
    handOffStoreLaunch(url, recentKey);
    RecentStore::add({ url, title, QStringLiteral("eagame"), thumb, recentKey });
    statusBar()->showMessage(tr("Launching “%1” via the EA app…").arg(title), 5000);
}

void MainWindow::launchXboxGame(const QString& key, const QString& uri, const QString& title, const QString& thumb)
{
    const QString id = XboxLibrary::idFrom(key, uri);   // the AUMID "<PFN>!<AppId>"
    const QString url = XboxLibrary::launchUri(id);     // rebuilt from the id: a recorded string is never replayed raw
    if (url.isEmpty())
    {
        statusBar()->showMessage(tr("No playable file is associated with “%1” yet.").arg(title), kFeedbackLong);
        return;
    }
    const QString recentKey = QStringLiteral("xbox:") + id;
    // The same one store hand-off as EA: under EB_UITEST_STORE_URI_SINK it logs the string as held back instead
    // of starting the game, which is how a UI-test drive proves this dispatch without the Xbox app. (No play-time
    // watch: LaunchWatch knows steam:// and the Epic URI only, so this is a fire-and-forget handoff.)
    handOffStoreLaunch(url, recentKey);
    RecentStore::add({ url, title, QStringLiteral("xboxgame"), thumb, recentKey });
    statusBar()->showMessage(tr("Launching “%1” via Xbox…").arg(title), 5000);
}

void MainWindow::openLibraryItem(const MediaItem& item)
{
    // A "Choose source…" fan-out still out from the PREVIOUS item is now stale: its reply must not clear the
    // notice this open raises, nor pop a picker for the old episode over what is playing now.
    bumpChooseSourceGen();
    // Whether the player/reader should offer "Issue with Streaming" for this item (an Allarr-resolved file
    // whose source can be swapped). Preserved across the remote-document download round-trip (item is copied).
    currentNextSourceCapable_ = item.nextSourceCapable;
    mwLog(QStringLiteral("open: \"%1\" type=%2 mime=%3 src=%4%5")
              .arg(item.title, item.type.isEmpty() ? QStringLiteral("?") : item.type,
                   item.mime.isEmpty() ? QStringLiteral("-") : item.mime, logSafeUrl(item.url),
                   splitTarget_ ? QStringLiteral(" [->split pane]") : QString()));
    // A Battle.net game: TWO routes on one mime, decided by whether the tile carries a url. A title with a known
    // product code carries NO url and launches the client by battlenet://<code> (fire-and-forget, the Epic shape);
    // a code-less title carries its resolved exe and runs through the MONITORED launchPcExe path (the GOG shape).
    // This must sit ABOVE the url.isEmpty() bail below — the coded route is precisely the url-less case.
    if (item.mime == QStringLiteral("battlenetgame"))
    {
        // The coded route: either a console tile (no url — the id is "bnet:<code>") or a row that already
        // carries the battlenet:// URI. Testing the SCHEME as well as emptiness keeps this safe by
        // construction: a coded Recent records the URI, and any surface that stamps that onto a tile's url
        // (a playlist entry, say) must still take the URI arm, never hand "battlenet://wow" to launchPcExe.
        const bool codedRoute = item.url.isEmpty() || item.url.startsWith(QStringLiteral("battlenet://"));
        if (codedRoute)
        {
            const QString code = item.id.startsWith(QStringLiteral("bnet:"))
                                     ? item.id.mid(QStringLiteral("bnet:").size()) : item.id;
            // Prefer an already-formed URI over rebuilding it from the id (a name-keyed code-less id would
            // otherwise produce battlenet://<DisplayName>, which is not a product code).
            const QString uri = item.url.startsWith(QStringLiteral("battlenet://"))
                                     ? item.url : BattleNetLibrary::launchUri(code);
            if (uri.isEmpty())
            {
                statusBar()->showMessage(tr("No playable file is associated with “%1” yet.").arg(item.title),
                                         kFeedbackLong);
                return;
            }
            QDesktopServices::openUrl(QUrl(uri));
            RecentStore::add({ uri, item.title, QStringLiteral("battlenetgame"), item.thumbnailUrl, item.id });
            statusBar()->showMessage(tr("Launching “%1” via Battle.net…").arg(item.title), 5000);
            return;
        }
        // Code-less: launchPcExe records the "battlenetgame" Recent ITSELF — do NOT re-record here.
        launchPcExe(item.url, item.id, item.title, item.thumbnailUrl, QStringLiteral("battlenetgame"));
        return;
    }
    // A Ubisoft Connect game (#60). Above the url.isEmpty() bail for the same reason as Battle.net: a playlist
    // row carries no url, only its "ubi:<id>" id, and the URI is built from that.
    if (item.mime == QStringLiteral("ubisoftgame")
        || item.url.startsWith(QStringLiteral("uplay://"), Qt::CaseInsensitive))
    {
        launchUbisoftGame(item.id, item.url, item.title, item.thumbnailUrl);
        return;
    }
    // An EA app game (#60 increment 2), above the url.isEmpty() bail for the same reason: a playlist row carries
    // only its "ea:<id>" id.
    if (item.mime == QStringLiteral("eagame")
        || item.url.startsWith(QStringLiteral("origin2://game/launch"), Qt::CaseInsensitive))
    {
        launchEaGame(item.id, item.url, item.title, item.thumbnailUrl);
        return;
    }
    // An Xbox game (#60 increment 3), above the url.isEmpty() bail for the same reason: a playlist row carries
    // only its "xbox:<AUMID>" id.
    if (item.mime == QStringLiteral("xboxgame")
        || item.url.startsWith(QStringLiteral("shell:AppsFolder\\"), Qt::CaseInsensitive))
    {
        launchXboxGame(item.id, item.url, item.title, item.thumbnailUrl);
        return;
    }
    if (item.url.isEmpty())
    {
        // Catalog metadata with no file associated yet (movies/games/episodes/tracks).
        statusBar()->showMessage(tr("No playable file is associated with “%1” yet.").arg(item.title), kFeedbackLong);
        return;
    }
    // Local library prefer-local: if we own this item on disk, play the local file instead of resolving
    // a stream. Guard on mime so the re-entry (with a filesystem url) doesn't recurse. Movies key on id;
    // episodes key on imdbStreamId ("ttShow:season:episode", matches the OwnedIndex episode key).
    if (item.mime != QStringLiteral("local:video"))
    {
        QString localPath = LocalLibrary::index().localPathFor(item.id);
        if (localPath.isEmpty() && !item.imdbStreamId.isEmpty())
            localPath = LocalLibrary::index().localPathFor(item.imdbStreamId);
        if (!localPath.isEmpty() && QFileInfo::exists(localPath))
        {
            MediaItem local = item;
            local.url = localPath;
            local.mime = QStringLiteral("local:video");
            // Re-pointing the url means re-deriving the headers, per the contract on MediaItem::requestHeaders.
            // A filesystem path is not an origin, so this always clears them and the call looks pointless —
            // which is exactly why it has to be here. This is the one place in the tree where a MediaItem is
            // copied onto a different url, and "the copy happened to be harmless" is not the invariant; going
            // through forPlayUrl is. The day this branch re-points to a URL instead, the guard is already in
            // place rather than being something someone has to remember.
            local.requestHeaders = StreamHeaders::forPlayUrl(item.requestHeaders, item.url, local.url);
            openLibraryItem(local);   // re-enter: filesystem url + local:video mime -> mpv branch
            return;
        }
    }
    // A GOG game: a DRM-free exe launched through the MONITORED path (launchPcExe records the "goggame" Recent
    // itself — do NOT re-record here). Its exe rides on item.url; id/title/thumb come from the tile.
    if (item.mime == QStringLiteral("goggame"))
    {
        launchPcExe(item.url, item.id, item.title, item.thumbnailUrl, QStringLiteral("goggame"));
        return;
    }
    // An Epic game: hand the launcher URI to the OS (fire-and-forget, exactly like steam://). Record a Recent
    // (kind "epicgame", key "epic:<AppName>") so it resumes from the Recent tab and re-launches via the URI.
    if (item.url.startsWith(QStringLiteral("com.epicgames.launcher://")))
    {
        handOffStoreLaunch(item.url, item.id);   // #61: the same openUrl, plus a play-time watch
        const QString appName = item.url.section(QLatin1Char('/'), -1).section(QLatin1Char('?'), 0, 0);
        const QString key = item.id.startsWith(QStringLiteral("epic:"))
                                ? item.id : QStringLiteral("epic:") + appName;
        RecentStore::add({ item.url, item.title, QStringLiteral("epicgame"), item.thumbnailUrl, key });
        statusBar()->showMessage(tr("Launching “%1” via Epic…").arg(item.title), 5000);
        return;
    }
    // A Steam game: hand it to the Steam client to launch (it handles install/run). Fire-and-forget — the Steam
    // client owns the process; #61 watches for the game to time the session (not for a steam://install/).
    if (item.url.startsWith(QStringLiteral("steam://")))
    {
        handOffStoreLaunch(item.url, item.id);
        const bool installing = item.url.startsWith(QStringLiteral("steam://install/"));
        // A RUN records a Recent (kind "steamgame", key "steam:<appid>", capsule thumb) so it resumes from the
        // Recent tab and re-launches via SteamLibrary::launchUrl. An install handoff is not a play -> no Recent.
        if (!installing)
        {
            const QString appid = item.url.section(QLatin1Char('/'), -1);
            const QString key = item.id.startsWith(QStringLiteral("steam:"))
                                    ? item.id : QStringLiteral("steam:") + appid;
            RecentStore::add({ item.url, item.title, QStringLiteral("steamgame"), item.thumbnailUrl, key });
        }
        statusBar()->showMessage(installing ? tr("Installing “%1” via Steam…").arg(item.title)
                                            : tr("Launching “%1” via Steam…").arg(item.title), 5000);
        return;
    }
    const QString url = item.url;
    const QString type = item.type.toLower();
    const QString lower = url.toLower();
    QString err;

    // PC (Windows) games aren't emulator ROMs — we're on a PC, so download the file and hand it to the OS to
    // run/install (an installer or portable .exe runs; an archive opens). Must come before the ROM handling
    // below, which would otherwise try to find an emulator core for it and fail ("no system").
    if (type == QStringLiteral("game") && isPcPlatform(item.systemHint))
    {
        openPcGame(item);
        return;
    }

    // Playlists (IPTV channel lists / HLS streams) must be handled before the ROM check below: ".m3u" is
    // also the PlayStation multi-disc extension, so it would otherwise be fetched as a game. streams_->resolve()
    // re-checks the contents and still routes a genuine disc list to the emulator.
    if (StreamResolver::isM3uRef(lower))
    {
        if (splitTarget_) { splitTarget_->openVideo(url, item.title, item.requestHeaders); finishSplitOpen(); return; }
        // The item's own proxyHeaders ride along: this is a plain HTTP fetch of the STREAM URL, so a gated
        // HLS/IPTV source is refused here — and refused before playback, so it would read as a broken
        // playlist rather than a missing header.
        streams_->resolve(url, item.title, item.requestHeaders);
        return;
    }

    // Documents (CBZ/EPUB/PDF) open through file-based readers (miniz / epub / PDFium), which need a
    // local path. When an addon hands us a remote http(s) document, fetch it to a cache file first,
    // then re-enter with the local path. Pick the reader extension from the url, else the mime (debrid
    // links often have no extension), else the media type. Audio mimes/types are left for libmpv.
    if (lower.startsWith(QStringLiteral("http://")) || lower.startsWith(QStringLiteral("https://")))
    {
        const QString mime = item.mime.toLower();
        QString ext;
        if      (lower.endsWith(QStringLiteral(".cbz")))  ext = QStringLiteral(".cbz");
        else if (lower.endsWith(QStringLiteral(".epub"))) ext = QStringLiteral(".epub");
        else if (lower.endsWith(QStringLiteral(".pdf")))  ext = QStringLiteral(".pdf");
        else if (mime.contains(QStringLiteral("epub")))   ext = QStringLiteral(".epub");
        else if (mime.contains(QStringLiteral("pdf")))    ext = QStringLiteral(".pdf");
        else if (mime.contains(QStringLiteral("comicbook")) || mime.contains(QStringLiteral("cbz"))) ext = QStringLiteral(".cbz");
        else if (type == QStringLiteral("comic") || type == QStringLiteral("manga")) ext = QStringLiteral(".cbz");
        else if (type == QStringLiteral("ebook") || type == QStringLiteral("book")) ext = QStringLiteral(".epub");
        else if (type == QStringLiteral("pdf"))  ext = QStringLiteral(".pdf");
        if (!ext.isEmpty()) { fetchRemoteDocumentThenOpen(item, ext); return; }

        // Game ROMs run in the emulator (RetroView / external), which loads a local file. Fetch the
        // ROM to a cache file first, keeping its extension so the right core is picked. Take the extension
        // from the url, else the mime — a debrid link (e.g. a Switch game) has no filename in its path, so
        // the addon passes the rom type as an "application/x-<ext>" mime (mirrors the document fallback above).
        QString romExt = QFileInfo(QUrl(url).path()).suffix().toLower();
        if (romExt.isEmpty() && type == QStringLiteral("game"))
        {
            QString cand = mime.section(QLatin1Char('/'), -1).section(QLatin1Char(';'), 0, 0);
            if (cand.startsWith(QStringLiteral("x-"))) cand = cand.mid(2);
            // A recognized ROM extension, OR an archive the ROM is packed in (a debrid link is a bare id with
            // no filename, so the mime is our only clue — console ROMs are usually a .zip/.7z of the ROM, which
            // openGamePath extracts). Without this the raw URL is handed to the emulator, which can't open it.
            if (SystemCatalog::forExtension(cand) != nullptr
                || cand == QStringLiteral("zip") || cand == QStringLiteral("7z") || cand == QStringLiteral("rar"))
                romExt = cand;
        }
        if (!romExt.isEmpty() && (type == QStringLiteral("game") || SystemCatalog::forExtension(romExt) != nullptr))
        { fetchRemoteDocumentThenOpen(item, QStringLiteral(".") + romExt); return; }
    }

    // An audio leaf whose payload PLAINLY cannot be audio (#207). Asking for an audiobook once resolved to an
    // ebook release, whose debrid link is a "zip the whole thing" endpoint; it was handed to the player as
    // audio, and what the user got was a player that came up and did nothing. A dead player teaches people the
    // app is broken. A sentence tells them the release was wrong, which is the true and useful thing to say.
    //
    // This is the cheap, honest check and nothing more: url and mime, no fetch, no sniffing. `Unknown` — which
    // is most links — plays exactly as before, so the only behaviour that changes is the case that could not
    // have worked. It sits ABOVE the split-screen block because a pane plays the same nothing that the
    // full-screen player does, and BELOW the document/ROM fetch above, which already gives a `.epub` url its
    // reader rather than its player.
    if (type == QStringLiteral("audiobook") || type == QStringLiteral("audio"))
    {
        const CatalogMatch::PayloadShape shape = CatalogMatch::payloadShape(url, item.mime);
        if (shape == CatalogMatch::PayloadShape::Document || shape == CatalogMatch::PayloadShape::Archive)
        {
            mwLog(QStringLiteral("open: refusing to play \"%1\" — payload is %2, not audio")
                      .arg(item.title, shape == CatalogMatch::PayloadShape::Document
                                           ? QStringLiteral("a document") : QStringLiteral("an archive")));
            notify(shape == CatalogMatch::PayloadShape::Document
                       ? tr("“%1” can't be played: the copy that was found is a document, not an audio file. "
                            "Try another source.").arg(item.title)
                       : tr("“%1” can't be played: the copy that was found is an archive, not an audio file. "
                            "Try another source.").arg(item.title),
                   kFeedbackLong);
            return;
        }
    }

    // Split screen: the item is now a local file (remote docs/ROMs were fetched above) or a streamable URL -
    // load it into the focused pane instead of the full-screen views. Games fall through to openGamePath,
    // which is split-aware (it needs to resolve the core first).
    if (splitTarget_)
    {
        // Hoisted from the video branch below so it can gate the supersede as well: every non-game leaf in
        // this block hands the item to the pane, which then owns the screen, so all of them must cancel a
        // pending external launch. A GAME must not — it falls through to openGamePath, whose external route
        // goes back through open() -> runEmulator, where the busy-refusal is the behaviour we deliberately
        // keep (superseding would let two emulator install runs race on the same directory).
        const bool isGame = (type == QStringLiteral("game")
                             || SystemCatalog::forExtension(QFileInfo(lower).suffix()) != nullptr);
        if (!isGame) supersedePendingExternalLaunch();
        if (type == QStringLiteral("ebook") || EbookFormats::opensInBookReader(lower))
        { splitTarget_->openBook(url); finishSplitOpen(); return; }
        if (type == QStringLiteral("pdf") || lower.endsWith(QStringLiteral(".pdf")))
        { splitTarget_->openPdf(url); finishSplitOpen(); return; }
        if (lower.endsWith(QStringLiteral(".cbz")) || lower.endsWith(QStringLiteral(".cb7"))
            || lower.endsWith(QStringLiteral(".cbt")) || lower.endsWith(QStringLiteral(".cbr")))
        { splitTarget_->openComic(url); finishSplitOpen(); return; }
        if (PhotoLibrary::isPhotoFile(url)) // #102: a local image opens in the pane's photo viewer
        { splitTarget_->openPhoto(url); finishSplitOpen(); return; }
        if (!isGame) // video / audio / audiobook all play through the pane's own libmpv
        { splitTarget_->openVideo(url, item.title, item.requestHeaders); finishSplitOpen(); return; }
    }

    // Recent entry for a document: carry the catalog title/cover (the local file is often a hashed cache
    // name) and key on the stable item id so re-opening de-dups instead of stacking a second entry.
    auto recordDocument = [&] {
        const QString t = item.title.isEmpty() ? QFileInfo(url).completeBaseName() : item.title;
        RecentItem row{ url, t, QStringLiteral("document"), item.thumbnailUrl, item.id };
        // WHO TO ASK ABOUT THIS ITEM LATER, which a document row has never carried. applyRemintRecipe
        // writes a source only for a row whose path is a network link, and a cached comic's path is a
        // file — so a resumed volume had an item id and nobody to ask it of, and could never rebuild the
        // list of what comes next.
        //
        // This is NOT a re-mint recipe and must not be read as one: the direct route needs sourceRoute
        // and sourceType as well, both left empty here, so reopenFor still refuses it and replays the
        // path exactly as it does today. #224's "all three fields or none" rule is about those three.
        row.sourceAddonId = item.sourceAddonId;
        // WHICH READING CATALOGUE THIS ROW BELONGS TO. `kind` is the routing kind and is "document" for an
        // EPUB, a PDF, a comic issue and a manga chapter alike — they all open in the reader stack — so the
        // three reading catalogues had no way to tell their Recent folders apart and each showed all of it.
        // Taken from the item's OWN type, so it is recorded rather than guessed; empty for a type that says
        // nothing, which core::matchesReadingScope keeps visible everywhere rather than dropping.
        row.form = core::readingForm(item.type);
        RecentStore::add(row);
    };

    if (type == QStringLiteral("ebook") || EbookFormats::opensInBookReader(lower))
    {
        if (!book_->openBook(url, &err)) { notify(tr("Can't open book: %1").arg(err), kFeedbackLong); return; }
        partPlaybackForReader(); pdf_->persist(); comic_->persist();
        book_->setStreamIssueVisible(currentNextSourceCapable_); // remote (Allarr) books can swap source
        presentBook();
        recordDocument();
    }
    else if (type == QStringLiteral("pdf") || lower.endsWith(QStringLiteral(".pdf")))
    {
        // Prefer the reflowable reader (font sizing / pagination like EPUB) for text PDFs - this is mainly a
        // book app. Fall back to the fixed page-image view for scanned PDFs that have no text layer.
        if (book_->openBook(url, &err))
        {
            partPlaybackForReader(); pdf_->persist(); comic_->persist();
            book_->setStreamIssueVisible(currentNextSourceCapable_);
            presentBook();
            recordDocument();
        }
        else if (pdf_->openPdf(url, &err))
        {
            partPlaybackForReader(); book_->persist(); comic_->persist();
            pdf_->setStreamIssueVisible(currentNextSourceCapable_); // remote (Allarr) books can swap source
            presentPdf();
            recordDocument();
        }
        else { notify(tr("Can't open PDF: %1").arg(err), kFeedbackLong); }
    }
    else if (lower.endsWith(QStringLiteral(".cbz")) || lower.endsWith(QStringLiteral(".cb7"))
             || lower.endsWith(QStringLiteral(".cbt"))
             || lower.endsWith(QStringLiteral(".cbr"))) // a downloaded/associated comic archive
    {
        if (!comic_->openComic(url, &err)) { notify(tr("Can't open comic: %1").arg(err), kFeedbackLong); return; }
        partPlaybackForReader(); book_->persist(); pdf_->persist();
        // A run the OPEN brought with it wins over one derived from the folder: it names real neighbours
        // from the list this issue was opened from, where the folder — for a provider-fetched volume — is
        // the app's own cache and yields nothing at all (see ChapterOrder::isCachePath).
        if (item.chapterRun.isValid()) armComicRun(item.chapterRun);
        else
        {
            // No list came with the open — a Recent, or a path that never had one. The folder run is
            // still the right answer for a comic filed in a folder, and yields nothing inside the app's
            // own cache; the catalog rebuild is what gives a provider-fetched volume its neighbours.
            armComicRun(folderRunFor(url));
            rebuildCatalogRun(item);
        }
        presentComic();
        recordDocument();
    }
    else if (PhotoLibrary::isPhotoFile(url)) // a local image (issue #102): view it + page its folder
    {
        // The viewer is ComicView in photo mode — the same render/page/zoom widget over the file's siblings in
        // its folder, opened on the picked image. Presented through the comic surface it shares.
        const QString folder = QFileInfo(url).absolutePath();
        if (!comic_->openFolder(folder, url, &err)) { notify(tr("Can't open photo: %1").arg(err), kFeedbackLong); return; }
        armComicRun(ChapterRun{}); // a photo folder is not a series (issue #102)
        partPlaybackForReader(); book_->persist(); pdf_->persist();
        presentComic();
        recordDocument();
    }
    else if (type == QStringLiteral("audiobook") || item.mime.toLower().startsWith(QStringLiteral("audio/")))
    {
        // An audiobook (or any audio-mime stream, e.g. from Allarr): play in the now-playing audio view with
        // resume keyed by the stable item id (a re-resolved debrid URL changes, so it can't be the key).
        noteStreamScrobble(item, type);   // #192: this leaf is where the item's tags are still in scope
        // #214: a release that turned out to be MANY FILES is a BOOK, not a link. The resolve that got us
        // here already asked the source for its parts, so this is a read of what it found rather than a
        // second conversation. One part or none means a single file, which takes the untouched path below —
        // an .m4b with its chapters inside already worked, and must not change key underneath anybody.
        if (item.bookParts.size() > 1) { openRemoteAudiobook(item, url); return; }
        // #224: the item rides along so the Recents row this writes carries a re-mint recipe. The multi-part
        // branch above already wrote one (openRemoteAudiobook's own row); this is the single-file half of the
        // same book, and the two must not disagree about whether the row can be re-opened tomorrow.
        openAudioStream(url, item.id, item.title, item.thumbnailUrl, item.requestHeaders, &item);
    }
    else if (type == QStringLiteral("audio"))
    {
        // Delegate to the SAME entry point the audiobook branch uses: openAudioStream owns the themed-audio
        // routing (themedAudioSession_ + the page's cover/title data) as well as the J18 stable-id resume key.
        // The old inline setQueue bypassed that routing, leaving a STALE themedAudioSession_ to decide the
        // surface — a classic page in themed mode (or a themed page with the previous item's art).
        noteStreamScrobble(item, type);   // #192: the same note, from the music half of the same leaf
        // #224: and the recipe with it. A remote track resolved through a file provider's /stream was ALREADY
        // stamped with its sourceAddonId upstream (HomeView's resolveStream leaf) — the stamp was simply
        // dropped on the floor here, because this sink wrote the row without consulting the item.
        openAudioStream(url, item.id, item.title, item.thumbnailUrl, item.requestHeaders, &item);
    }
    else if (type == QStringLiteral("game") || SystemCatalog::forExtension(QFileInfo(lower).suffix()) != nullptr)
    {
        // Carry the catalog title/cover/id into Recent (the ROM file itself is a hashed cache name), and the
        // console/platform hint so the right emulator is picked even when the extension is shared.
        openGamePath(url, item.title, item.thumbnailUrl, item.id, item.systemHint);
    }
    else // "video", "link", or anything else playable -> libmpv (handles files and http/streams)
    {
        // Top of the leaf, above the routePlay handoff below, which RETURNS when an external player takes
        // the item: this is about to be watched here or in VLC either way, so a pending external launch is
        // superseded on both routes. (The audio/audiobook leaves above need no call — they delegate to
        // openAudioStream, which carries its own.)
        supersedePendingExternalLaunch();
        // Resume + Recent are keyed by the item's stable id when it has one (a debrid/stream URL changes every
        // time it's resolved, so keying on the URL would lose your place and duplicate the Recent entry).
        const QString rkey = item.id.isEmpty() ? url : item.id;
        // External-player handoff: an external player takes the resolved URL (Recent on both routes). A one-off
        // armed on this leaf rode here as item.playRouteHint (leak-free — a failed resolve never reaches here).
        //
        // A source that needs HTTP headers never goes out to an external player: we have no way to make the
        // headers follow (VLC would take them on a command line the whole machine can read, MPC-HC has no
        // equivalent, an Android ACTION_VIEW intent has nowhere to put them), so the handoff would 403 and
        // the user would see nothing happen. Keep it in the built-in player, which CAN satisfy the gate, and
        // say why — refusing outright would cost them the stream to protect a routing preference.
        //
        // ONE routePlay call either way, dry-run or not: it consumes the one-shot playRouteOverride_, so
        // skipping it entirely would leave a "play externally" one-off armed for whatever plays next.
        const bool headerGated = StreamHeaders::externalRoute(item.requestHeaders)
                                 == StreamHeaders::ExternalRoute::FallBackToBuiltin;
        const bool routedOut = routePlay(url, routeFromHint(item.playRouteHint), /*dryRun=*/headerGated);
        if (routedOut && !headerGated) {
            const QString rt = !item.title.isEmpty() ? item.title : QUrl(url).fileName();
            RecentItem row{ url, rt, QStringLiteral("video"), item.thumbnailUrl, rkey };
            applyRemintRecipe(row, item);
            RecentStore::add(row);
            return;
        }
        if (routedOut && headerGated)
            notify(tr("This source needs custom HTTP headers, which can't be passed to an external player — "
                      "playing it here instead."), kFeedbackLong);
        notePlaybackStart();     // channel guard (built-in catalog play): keep the channel iff this is its pick
        retro_->stop(); book_->persist(); pdf_->persist(); comic_->persist(); session_->clearQueue();
        session_->setMediaVideo(true); // consumption-stats: a catalog movie/episode stream accrues "watch" seconds
        session_->beginResume(rkey);
        syncKey_ = rkey;         // catalog stream: key sync offsets by the stable id, not the volatile URL
        armSubtitleFetch(item); // auto-download a subtitle if this movie/episode has none in the preferred language
        castUrl_ = url; castTitle_ = item.title; castMime_ = item.mime; // castable stream for the cast button
        // A cast device fetches the URL itself, so it is the external-player problem again: the headers
        // cannot follow, and the device would sit on a black screen. Remember that this stream is gated so
        // the cast menu can say so instead of offering a cast that cannot work.
        castHeaderGated_ = !item.requestHeaders.isEmpty();
        castMgr_->startDiscovery();     // prime device discovery so the cast menu is populated when opened
        // Trakt: begin tracking this movie/episode. NOTE: local-library items now carry an imdbStreamId too
        // (added for subtitle matching), so files played off disk scrobble to Trakt as well — previously only
        // catalog streams did. That's the desirable behaviour (your watch history shouldn't depend on source).
        startScrobble(item.imdbStreamId);
        // #156: the tracker needs a TITLE to search AniList with, and scrobbleImdb_ carries only an id.
        // Captured here, beside the id it belongs to, rather than looked up later from a member that by
        // then describes whatever is playing now.
        trackerVideoTitle_ = item.title;
        // #Discord: the same seam, but the id comes from the ITEM rather than from scrobbleImdb_ -
        // startScrobble() early-returns when Trakt is not connected, so reading the member back would hand
        // the IMDb button to Trakt users only. Everything else here is already on the item; nothing is
        // looked up.
        if (presence_) {
            Presence::Item pi;
            pi.kind = item.type == QLatin1String("livetv")  ? Presence::Kind::LiveTv
                    : (item.type == QLatin1String("episode")
                       || item.imdbStreamId.contains(QLatin1Char(':'))) ? Presence::Kind::Episode
                                                                        : Presence::Kind::Movie;
            pi.title    = item.title;
            pi.subtitle = pi.kind == Presence::Kind::LiveTv ? tr("Live TV") : item.subtitle;
            pi.artUrl   = item.thumbnailUrl;
            pi.imdbId   = item.imdbStreamId;
            presence_->setItem(pi);
        }
        stack_->setCurrentWidget(playerPage_);
        player_->play(url, item.requestHeaders);
        revealMediaControls();
        const QString title = !item.title.isEmpty() ? item.title : QUrl(url).fileName();
        RecentItem row{ url, title, QStringLiteral("video"), item.thumbnailUrl, rkey };
        applyRemintRecipe(row, item);
        RecentStore::add(row);
    }
}
