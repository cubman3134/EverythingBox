// PANELS (issue #186, increment 3a): MainWindow::showThemedXmb(), MainWindow::openAppearance() and
// MainWindow::showSubtitleMenu(), in their own translation unit.
//
// WHY THEY MOVED. MainWindow.cpp is the build's single longest compile (a TU compiles on one core however
// many /MP gives the rest) and it already needs /bigobj. Increment 1 took openGeneralSettings() out; these
// three are the next large, self-contained bodies: the themed XMB home, the Appearance page (BOTH its
// builders, themed PanelRows and classic QWidgets) and the player's Audio & Subtitles card.
//
// A PURE MOVE. Each function below is byte-for-byte what it was in MainWindow.cpp, leading comment included.
// So is formatMs(), a file-static only showSubtitleMenu() used, which moved with it unchanged. store(), which
// the rest of MainWindow.cpp shares, comes from MainWindowInternal.h.
//
// THE GUARD. showThemedXmb() and openAppearance() sat inside MainWindow.cpp's `#ifdef EB_HAVE_QML` region, so
// they sit inside the same condition here. openAppearance()'s no-QML stub moved into this file's `#else`, so
// each configuration still defines it exactly once; the region's other stubs stayed in MainWindow.cpp.
// showThemedXmb() has no stub, as before. showSubtitleMenu() was never guarded and is not guarded here.
#include "MainWindow.h"
#include "MainWindowInternal.h"   // store(): shared with MainWindow.cpp, defined once

#include "FeedbackPolicy.h"      // kFeedbackLong, kUiFadeMs
#include "HomeView.h"
#include "RegistryBrowser.h"

#include "../addons/AddonManager.h"
#include "../addons/StremioTranslate.h"
#include "../browse/LeafRoute.h"
#include "../core/ProfileStore.h"
#include "../core/Settings.h"
#include "../core/SubtitleCache.h"
#include "../core/SubtitleFetcher.h"
#include "../core/SyncOffsets.h"
#include "../core/ThemeChoice.h"
#include "../core/ThemeFormFactors.h"
#include "../theme2/FormFactor.h"
#include "../media/PlaybackSession.h"
#include "../theme2/PanelModel.h"
#include "../video/MpvWidget.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#ifdef EB_HAVE_QML
#include "../theme2/ThemeEngine.h"
#include "../theme2/ThemedPanelHost.h"
#include "../theme2/ThemePickerHost.h"
#include <QQuickItem>
#include <QQuickWidget>
#endif

#ifdef EB_HAVE_QML
void MainWindow::showThemedXmb()
{
    themedHomeIsXmb_ = true;
    themedXmbInCatalog_ = false;

    // Debounce the live-metadata addon fetch: while you scroll the column the panel tracks instantly off a
    // skeleton, and only the row you settle on triggers the (networked) synopsis/facts fetch. ONE timer,
    // shared with the grid browse path (see ensureThemedMetaTimer).
    ensureThemedMetaTimer();

    QVariantList cats = home_->categoryItems(); // the buckets that have catalogs (each {title,key,glyph,accent})
    themedXmbCatKeys_.clear();
    for (const QVariant& v : cats) themedXmbCatKeys_ << v.toMap().value(QStringLiteral("key")).toString();
    cats << QVariantMap{ { QStringLiteral("title"), tr("Profiles") }, { QStringLiteral("glyph"), QStringLiteral("profiles") },
                         { QStringLiteral("accent"), QStringLiteral("#3E6FB8") } };
    themedXmbCatKeys_ << QStringLiteral("profiles");
    const int profilesIdx = int(cats.size()) - 1;
    cats << QVariantMap{ { QStringLiteral("title"), tr("Settings") }, { QStringLiteral("glyph"), QStringLiteral("settings") },
                         { QStringLiteral("accent"), QStringLiteral("#5B6470") } };
    themedXmbCatKeys_ << QStringLiteral("settings");
    const int settingsIdx = int(cats.size()) - 1;

    // The Profiles column: every profile (the current one ticked) to scroll + select, plus an add/edit entry.
    auto profilesColumn = [] {
        QVariantList out;
        const QString cur = ProfileStore::currentId();
        for (const Profile& p : ProfileStore::list())
            out << QVariantMap{
                { QStringLiteral("title"), (p.icon.isEmpty() ? QString() : p.icon + QStringLiteral("  ")) + p.name
                                           + (p.id == cur ? QStringLiteral("   ✓") : QString()) },
                { QStringLiteral("profileId"), p.id }, { QStringLiteral("accent"), QStringLiteral("#3E6FB8") } };
        out << QVariantMap{ { QStringLiteral("title"), QObject::tr("＋  Add / edit profiles…") },
                            { QStringLiteral("profileAction"), QStringLiteral("manage") },
                            { QStringLiteral("accent"), QStringLiteral("#5B6470") } };
        return out;
    };

    const QStringList themes = ThemeEngine::availableThemes();
    const QString themeName = currentThemeFolder();
    const QString themeDir = ThemeEngine::themesRoot() + QStringLiteral("/") + themeName;
    const int startCat = qBound(0, themedHomeIndex_, settingsIdx);

    QVariantMap system; system.insert(QStringLiteral("name"), QStringLiteral("EverythingBox"));

    // Show a bucket's catalog list as the column. If the bucket has a single catalog (e.g. Games -> one
    // catalog whose top level IS the console list), open it directly so the column shows its contents (the
    // consoles), skipping a pointless one-item folder.
    auto showCatalogs = [this, profilesColumn](int cat, int selectIdx) {
        const QString key = (cat >= 0 && cat < themedXmbCatKeys_.size()) ? themedXmbCatKeys_[cat] : QString();
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        // Leaving the items level (to a catalog list / profiles / settings): no leaf is selected, so drop the
        // metadata panel and any open action chooser. (When a catalog loads, browseItemsChanged repopulates it.)
        if (r) { r->setProperty("selectedMeta", QVariantMap()); r->setProperty("actionsOpen", false);
                 r->setProperty("catLoading", false); } // default: no spinner (the async branches below turn it on)
        if (key == QStringLiteral("profiles")) // the Profiles column: scroll to a profile to select it
        {
            themedXmbInCatalog_ = false; themedXmbAutoOpened_ = false;
            themedXmbCatalogs_ = profilesColumn();
            int sel = selectIdx; const QString cur = ProfileStore::currentId();
            for (int i = 0; i < themedXmbCatalogs_.size(); ++i)
                if (themedXmbCatalogs_[i].toMap().value(QStringLiteral("profileId")).toString() == cur) { sel = i; break; }
            if (r) { r->setProperty("items", themedXmbCatalogs_); r->setProperty("currentIndex", qMax(0, sel)); }
            return;
        }
        themedXmbCatalogs_ = (key == QStringLiteral("settings")) ? QVariantList() : home_->categoryCatalogs(key);
        // Count real catalogs (navKey-bearing): the trailing Playlists folder has none, so a lone-catalog bucket
        // still auto-opens its single catalog rather than stranding on a two-row [catalog, Playlists] column.
        int realCatalogs = 0; QString onlyNavKey;
        for (const QVariant& v : themedXmbCatalogs_)
        {
            const QString nk = v.toMap().value(QStringLiteral("navKey")).toString();
            if (!nk.isEmpty()) { ++realCatalogs; onlyNavKey = nk; }
        }
        if (realCatalogs == 1) // single catalog -> open straight into its contents
        {
            themedXmbInCatalog_ = true;
            themedXmbAutoOpened_ = true;
            if (r) { r->setProperty("items", QVariantList()); r->setProperty("currentIndex", 0); r->setProperty("catLoading", true); } // clear + spinner while it loads
            home_->activateNav(onlyNavKey);
            return;
        }
        themedXmbInCatalog_ = false;
        themedXmbAutoOpened_ = false;
        const int sel = qBound(0, selectIdx, qMax(0, int(themedXmbCatalogs_.size()) - 1));
        if (r) { r->setProperty("items", themedXmbCatalogs_); r->setProperty("currentIndex", sel); }
    };

    auto onActivated = [this, settingsIdx, profilesIdx](int itemIdx) {
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        const int cat = r ? r->property("catIndex").toInt() : 0;
        // The full settings hub: Add-ons, Cloud Sync, Appearance, … Deferred (issue #28) because
        // openSettingsHub goes through parentalUnlock -> Osk::getText whenever a parental PIN is set on a
        // restricted profile, i.e. a nested event loop under the live column delegate, and then resets the
        // panel host's model and switches the page. Nothing to resolve: the column, not a row, is the target.
        if (cat == settingsIdx) { deferPastQmlEmission([this] { openSettingsHub(); }); return; }
        if (cat == profilesIdx) // the Profiles column: pick a profile to switch, or open the add/edit dialog
        {
            if (itemIdx < 0 || itemIdx >= themedXmbCatalogs_.size()) return;
            const QVariantMap m = themedXmbCatalogs_[itemIdx].toMap();
            const QString id = m.value(QStringLiteral("profileId")).toString();
            if (!id.isEmpty())
            {
                // A switch from HERE is the same switch the profile picker performs, so it goes through the same
                // door. A bare setCurrent()+rebuild used to live here and silently skipped everything chooseProfile
                // owns: the ItemMarks/ConsumptionStats invalidations (without them the new profile's home renders
                // against the PREVIOUS profile's hidden items / completion marks / tags and stats) and roadmap
                // #57's forced theme pick for a profile that has never chosen one. Never re-copy its body.
                //
                // "Stays on the Profiles category" still holds: chooseProfile finishes through openHome(), whose
                // themed rebuild starts at themedHomeIndex_ — already profilesIdx, because onCategory recorded it
                // when the user moved onto this column. Pin it anyway so the intent survives a future reorder.
                // /*startup*/ false: mid-session, so the theme step's Back means "keep the resolved default and go
                // home", not "quit". (chooseProfile also schedules maybeOfferTvMode; it is guard-bailing and
                // one-shot — auto display mode, not yet prompted, themed, full screen, no overlay — so firing it
                // on this route is as harmless as on the picker's, which has always done it.)
                //
                // DEFERRED A TURN (issue #28). This is the worst-shaped route on the XMB: chooseProfile opens
                // the passcode gate — PasscodePad::ask, a QEventLoop, re-entered in a for(;;) — and then
                // finishes through openHome() -> showThemedXmb(), which does
                // stack_->removeWidget(old) + old->deleteLater() on `old` == themedHome_, THE WIDGET WHOSE
                // COLUMN DELEGATE IS EMITTING activated() RIGHT NOW. Nested loop and self-retire in one route.
                // showHomeScreen() on the else branch is the same retire without the loop.
                //
                // itemIdx was already resolved to the PROFILE ID above, and the id is what crosses the turn —
                // the row index would be read against themedXmbCatalogs_, which showCatalogs rebuilds, and
                // switching to the wrong profile is not something a user can undo by pressing it again.
                deferPastQmlEmission([this, id, profilesIdx] {
                    themedHomeIndex_ = profilesIdx;
                    if (id != ProfileStore::currentId()) chooseProfile(id, /*startup*/ false);
                    else showHomeScreen(); // re-picking the current profile: nothing to switch, rebuild in place
                });
            }
            else if (m.value(QStringLiteral("profileAction")).toString() == QStringLiteral("manage"))
            {
                // Deferred for the same reason as its sibling above, one step shallower: onSwitchProfile goes
                // through parentalUnlock -> Osk::getText (a nested loop) before it presents the picker.
                deferPastQmlEmission([this, profilesIdx] {
                    themedHomeIndex_ = profilesIdx; // return here after the dialog
                    onSwitchProfile();
                });
            }
            return;
        }
        if (!themedXmbInCatalog_) // the column is the catalog list -> open the chosen catalog into its items
        {
            if (itemIdx < 0 || itemIdx >= themedXmbCatalogs_.size()) return;
            const QVariantMap sel = themedXmbCatalogs_[itemIdx].toMap();
            const QString plCategory = sel.value(QStringLiteral("playlistsCategory")).toString();
            if (!plCategory.isEmpty()) // the category-level Playlists folder -> drill its lists as a new root
            {
                themedXmbCatalogIndex_ = itemIdx;  // Back re-selects the Playlists row in the bucket column
                themedXmbInCatalog_ = true;
                themedXmbAutoOpened_ = false;       // a "catalog" nav level backs out to the bucket column
                if (r) r->setProperty("catLoading", true); // spinner until the deferred open lands
                // Defer the actual open one event-loop tick. openPlaylistsLevel fills the column SYNCHRONOUSLY
                // (unlike a real catalog's async fetch); firing it inside this activation handler would race the
                // QML nav model's own activate() transition and strand focus on the bucket column. A queued call
                // lets the QML settle first, exactly mirroring how an async catalog's items arrive next tick.
                QTimer::singleShot(0, this, [this, plCategory] {
                    home_->openPlaylistsLevel(plCategory, /*asRoot*/ true);
                    syncThemedLevels();
                });
                return;
            }
            const QString navKey = sel.value(QStringLiteral("navKey")).toString();
            if (navKey.isEmpty()) return;
            themedXmbCatalogIndex_ = itemIdx;       // remember the catalog, so Back re-selects it in the list
            themedXmbInCatalog_ = true;
            if (r) { r->setProperty("currentIndex", 0); r->setProperty("catLoading", true); } // spinner while it loads
            home_->activateNav(navKey); // its items land via browseItemsChanged (which now targets this column)
            syncThemedLevels(); // entered a catalog: push its "catalog" level now (before the async items land)
        }
        else if (home_->atRecentsLevel() || home_->atDownloadsLevel())
            home_->browseActivate(itemIdx); // a Recent/Downloaded row -> re-open the local file at its spot
        else // inside a catalog: containers drill in-column; a leaf opens the inline action chooser
        {
            const QVariantList col = r ? r->property("items").toList() : QVariantList();
            const QVariantMap m = (itemIdx >= 0 && itemIdx < col.size()) ? col[itemIdx].toMap() : QVariantMap();
            const bool expandable = m.value(QStringLiteral("expandable")).toBool();
            const QString rowType = m.value(QStringLiteral("type")).toString();
            // A container (series/console/volume) or a synthetic row (Playlists folder, a playlist, New)
            // drills / acts via the normal path; a real leaf opens the inline Play / Favorite /
            // Add-to-playlist chooser; a guidance row ("info") — the sentence saying why this level is empty,
            // which browseItems lets through ALONE so a themed column is never simply blank — drills too, and
            // is inert there because activateItem refuses type "info". Offering Play over a line of prose is
            // nonsense, and that Play could only ever answer "Nothing to play".
            //
            // The fork itself lives in browse::themedEnterFor, not here, so probe_leafroute can state it
            // headlessly: it is the first half of "a local leaf activated through the themed path reaches a
            // player", and a leaf that never reaches the chooser never reaches playThemedLeaf at all.
            if (browse::themedEnterFor(rowType, expandable) == browse::ThemedEnter::Drill)
                { home_->browseActivate(itemIdx); syncThemedLevels(); } // drilled -> a browse level
            else if (r)
            {
                r->setProperty("actionItem", itemIdx);
                r->setProperty("actionFav", home_->isThemedLeafFavorite(itemIdx));
                // #372: Download only where the press can act — HomeView::themedDownloadOffered, the answer the
                // detail view's action row reads, so the chooser and the detail row cannot disagree.
                r->setProperty("actionDownload", home_->themedDownloadOffered(itemIdx));
                // Romhacks only where the leaf is a retro game with a resolvable system — the same gate the
                // detail page's verb uses, asked here so the chooser can add the row.
                r->setProperty("actionRomhack", home_->romhackTargetAt(itemIdx, nullptr, nullptr));
                // #193 inc 2: the queue verbs, on a local music TRACK leaf. Asked with the same
                // "should this row be offered" shape as Romhacks above, and answered by the same table
                // Enter reads (browse::queueTargetFor). An ALBUM never reaches here — it is a '_' row and
                // themedEnterFor drills it — which is why the browse context menu carries that case.
                r->setProperty("actionQueue", home_->browseQueueTarget(itemIdx, nullptr));
                // #233: a native port bound to THIS game. Asked with the same "should this row be offered"
                // shape as the two above; the answer is false on all but the handful of rows the port
                // catalog names, which is what makes this a per-GAME verb rather than a per-system one.
                r->setProperty("actionNativePort", home_->browseNativePort(itemIdx, nullptr, nullptr));
                r->setProperty("actionIndex", 0);
                r->setProperty("actionsOpen", true);
            }
        }
    };
    // The themed XMB back is now the NavGraph's level stack (kept in lockstep by syncThemedLevels): a deeper
    // browse drill is a "browse" level (onPop = home_->browseBack()); being inside a MULTI-catalog bucket is a
    // "catalog" level (onPop = themedCatalogPop_ below, re-showing the catalog list). When those are all
    // unwound, nav.back()'s rootBack lands HERE and brings up the app pause menu. A single-catalog bucket whose
    // contents ARE the root has no "catalog" level, so it too bottoms out straight to the pause menu.
    auto onBack = [this] { showEscMenu(); };
    // Pop of the "catalog" level: back out of the opened catalog, re-selecting it in the bucket's list (reads
    // the live category cursor, exactly as the old closure branch did).
    themedCatalogPop_ = [this, showCatalogs] {
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        const int cat = r ? r->property("catIndex").toInt() : 0;
        showCatalogs(cat, themedXmbCatalogIndex_);
    };
    // Deferred (issue #28): showThemedHome()/showThemedXmb() retire `old` == themedHome_, the widget whose
    // delegate is emitting cycleTheme(). See the grid home's copy for the full reasoning.
    auto onCycle = [this, themes, themeName] {
        if (themes.isEmpty()) return;
        const QString next = themes[(qMax(0, int(themes.indexOf(themeName))) + 1) % themes.size()];
        deferPastQmlEmission([this, next] {
            ThemeChoice::setForProfile(ProfileStore::currentId(), next);
            showThemedHome();
        });
    };
    // Deferred (issue #28): promptThemedSearch is Osk::getText, a nested event loop, and both tails re-source
    // a model — the in-catalog branch writes items straight back onto THIS column via browseItemsChanged, the
    // root branch swaps the page. Which branch is taken is decided synchronously, off the live
    // themedXmbInCatalog_, so the deferral cannot re-route the search; nothing index-shaped crosses the turn.
    auto onSearch = [this] {
        // Inside a catalog: scope the search to it. At the XMB root: search every add-on at once (cross-addon).
        if (themedXmbInCatalog_)
        {
            const QString scope = home_->browseTitle();
            deferPastQmlEmission([this, scope] {
                const QString q = promptThemedSearch(scope);
                // A non-console search resets HomeView's drill stack to the base level (doSearch): sync the
                // graph levels NOW, not just on the async browseItemsChanged, so a fast Back can't pop a
                // stale level.
                if (!q.isNull()) { home_->searchInBrowse(q); syncThemedLevels(); }
            });
        }
        else
        {
            deferPastQmlEmission([this] {
                const QString q = promptThemedSearch(tr("everything"));
                if (!q.isNull() && !q.trimmed().isEmpty()) { home_->searchEverything(q); showThemedBrowse(); }
            });
        }
    };
    auto onNearEnd = [this] { if (themedXmbInCatalog_ && home_->browseHasMore()) home_->browseLoadMore(); };
    auto onCategory = [this, showCatalogs] {
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        if (!r) return;
        themedHomeIndex_ = r->property("catIndex").toInt();
        themedXmbCatalogIndex_ = 0;                  // a different bucket starts at the top of its catalog list
        showCatalogs(themedHomeIndex_, 0); // switching bucket resets the column to its catalog list
        syncThemedLevels(); // bucket switch clears the old catalog/browse levels (or adds the new auto-opened one)
    };
    // The column selection moved: refresh the live metadata panel for the new row (only while in a catalog).
    auto onSelect = [this](int idx) { if (themedXmbInCatalog_) refreshThemedMeta(idx); };
    // The inline chooser fired: 0 = Play the leaf (and close), 1 = toggle Favorite (and stay, so the heart
    // updates in place; the metadata panel's "★ Favorited" follows too), 2 = Add to a playlist (and close),
    // 3 = Download the leaf for keeps (and close).
    //
    // NOT deferred, deliberately, and the reason is the whole shape of issue #28. Closing the chooser and
    // re-sourcing a model from inside this emission only QUEUES the delegates' destruction —
    // QQmlDelegateModel::release goes through deleteLater — so it is flushed after the emission unwinds,
    // which is why this has always been safe. What is NOT safe is a nested event loop between the two: it
    // flushes those pending DeferredDeletes early, while the Repeater that owns them is still being walked.
    // Row 2 was the one branch here that ran one (NavMenu::pick, then Osk::getText on "New playlist…"), and
    // it is now deferred inside HomeView::addBrowseItemToPlaylist — at the point where the index is resolved,
    // rather than out here where it is not.
    auto onAction = [this](int selected) {
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        if (!r) return;
        const int idx = r->property("actionItem").toInt();
        // `selected` is the row POSITION the nav zone highlighted; the row's stable action code comes out of
        // the list Xmb.qml published (see ThemeView.qml's actionCodes). The two were the same number while
        // every optional row sat at the end of a contiguous code space; they stopped being the same the
        // moment two optional rows could appear with a third absent between them. An empty list is a scene
        // that has not built the chooser, where position and code still coincide by construction.
        const QVariantList codes = r->property("actionCodes").toList();
        const int which = (selected >= 0 && selected < codes.size()) ? codes.at(selected).toInt() : selected;
        if (which == 0)      { r->setProperty("actionsOpen", false); home_->playThemedLeaf(idx); }
        else if (which == 2) { r->setProperty("actionsOpen", false); home_->addBrowseItemToPlaylist(idx); }
        else if (which == 3) { r->setProperty("actionsOpen", false); home_->downloadThemedLeaf(idx); }
        // Romhacks: close the chooser, resolve the leaf NOW while idx is still valid, then defer the overlay
        // a turn — showRomhacks opens NavMenu/NavConfirm, and a nested loop inside this QML emission is
        // exactly crash #28.
        else if (which == 4)
        {
            r->setProperty("actionsOpen", false);
            MediaItem target;
            QString targetSystem;
            if (!home_->romhackTargetAt(idx, &target, &targetSystem)) return;
            QMetaObject::invokeMethod(this, [this, target, targetSystem] {
                showRomhacks(target, targetSystem);
            }, Qt::QueuedConnection);
        }
        // #193 inc 2: 5 = add this track to the end of the queue, 6 = play it next. Resolved NOW, while idx
        // is still valid, then run — queueMusic opens no overlay and spins no nested loop (its whole output
        // is a session edit and a toast), so it needs no deferral; what it must not do is outlive the index,
        // which is why the target is taken here rather than captured.
        else if (which == 5 || which == 6)
        {
            r->setProperty("actionsOpen", false);
            browse::QueueTarget qt;
            if (home_->browseQueueTarget(idx, &qt)) queueMusic(qt, /*playNext*/ which == 6);
        }
        // #233: the native port for this game. Resolved NOW while idx is still valid, then deferred a turn —
        // showNativePort opens a NavConfirm, and a nested loop inside this QML emission is crash #28. Exactly
        // the romhack arm's shape, for exactly the romhack arm's reason.
        else if (which == 7)
        {
            r->setProperty("actionsOpen", false);
            MediaItem target;
            QString portId;
            if (!home_->browseNativePort(idx, &target, &portId)) return;
            QMetaObject::invokeMethod(this, [this, target, portId] {
                showNativePort(target, portId);
            }, Qt::QueuedConnection);
        }
        else
        {
            home_->favoriteThemedLeaf(idx);
            r->setProperty("actionFav", home_->isThemedLeafFavorite(idx));
        }
    };
    // "P" on the highlighted item: add it to a playlist (only while inside a catalogue, on a real media row).
    // The index is read here and resolved to the item inside addBrowseItemToPlaylist, which is where the
    // deferral past this emission lives — see there.
    auto onPlaylistAdd = [this] {
        if (!themedXmbInCatalog_) return;
        QQuickItem* r = ThemeEngine::rootItem(themedHome_);
        if (r) home_->addBrowseItemToPlaylist(r->property("currentIndex").toInt());
    };

    // "F" inside an XMB catalog opens the transient browse Filter menu (a no-op at the XMB category root).
    // "M" on the now-playing audio page (drawn over this very surface) opens the queue menu (#193) — deferred
    // past the QML emission, because it opens a NavMenu and that is a nested event loop under a live delegate.
    auto onButton = [this](const QString& v) {
        if (v == QStringLiteral("filter") && themedXmbInCatalog_) runThemedBrowseFilter();
        else if (v == QStringLiteral("queuemenu")) deferPastQmlEmission([this] { showQueueMenu(); });
        // #193 increment 4: the standing "something is playing" chip. Deferred past the QML emission
        // for the same reason the queue menu is — reopening the page pushes a nav level (issue #28).
        else if (v == QStringLiteral("nowplaying")) deferPastQmlEmission([this] { resumeNowPlayingPage(); });
    };
    QWidget* w = ThemeEngine::buildView(themeDir, QVariantList(), system, this,
                                        onActivated, onBack, onCycle, onSearch, onNearEnd, onCategory,
                                        onSelect, onAction, onPlaylistAdd, onButton,
                                        [this] { openThemedDetail(-1); },
                                        [this](const QString& v) { runThemedDetailAction(v); },
                                        [this](const QString& v) { runThemedAudioTransport(v); },
                                        [this](int row) { if (session_) session_->playIndex(row); },
                                        [this](int line) { seekToLyricLine(line); });
    if (QQuickItem* r = ThemeEngine::rootItem(w))
    {
        r->setProperty("categories", cats);
        r->setProperty("catIndex", startCat);
        r->setProperty("uiMotionMs", kUiFadeMs); // J05: XMB motion duration — FeedbackPolicy.h owns the value
    }
    QWidget* old = themedHome_;
    themedHome_ = w;
    applyThemeMusic(themeDir); // ship this theme's default menu music (used when the user has no music folder)
    updateThemedNowPlaying(); // seed the Triple theme's now-playing readout
    syncNowPlayingIndicator(); // …and #193's standing sign: this root is BRAND NEW and holds nothing yet
    stack_->addWidget(w);
    stack_->setCurrentWidget(w);
    focusThemedPage(w);
    if (old) { stack_->removeWidget(old); old->deleteLater(); }
    nudgeThemedHome(); // repaint the rebuilt themed home

    showCatalogs(startCat, themedXmbCatalogIndex_); // populate the starting bucket's catalog list (restore on rebuild)
    syncThemedLevels(); // establish the initial level stack (an auto-opened single catalog restores its drill depth)

    if (!themeWatcher_)
    {
        themeWatcher_ = new QFileSystemWatcher(this);
        connect(themeWatcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString&) {
            if (themedHomeEnabled() && stack_->currentWidget() == themedHome_)
                QTimer::singleShot(150, this, [this] {
                    if (themedHomeEnabled() && stack_->currentWidget() == themedHome_) showThemedHome();
                });
        });
    }
    themeWatcher_->removePaths(themeWatcher_->files());
    const QString themeFile = themeDir + QStringLiteral("/theme.json");
    if (QFile::exists(themeFile)) themeWatcher_->addPath(themeFile);
}

// The home theme picker, as a full-screen panel page in the main window (like the other settings screens).
// Changes save as you make them and preview live; backing out (-> the settings hub -> home) applies them.
void MainWindow::openAppearance()
{
    // A SETTINGS ENTRY POINT IN ITS OWN RIGHT — the only open* that is reachable without the hub: Ctrl+Shift+A
    // from anywhere, the grid home's Appearance tile, and a theme's own "appearance" button all land here. The
    // transaction must exist before the first theme write, so open it here too; begin() no-ops when the user
    // arrived the ordinary way (hub -> Appearance), so the visit still shares ONE transaction.
    enterSettingsArea();
    // Themed mode: render Appearance as a flat PanelRow list on the Nav Contract (ThemedPanelHost), instead of
    // the classic QWidget builder — the last classic surface reachable in themed mode (B2 Task 6.75). Same setters
    // verbatim: the themed-home Toggle writes themedHome/enabled; the theme Choice goes through ThemeChoice,
    // which owns the per-profile key and syncs for us.
    //
    // PREVIEW: the classic panel embedded a live QQuickWidget preview of the picked theme. In themed mode the app
    // ITSELF is theme-rendered, so apply-on-select IS the preview — picking a theme restyles the LIVE panel host
    // to that theme's settingsPanel block (setStyle(settingsPanelStyle()), which reads the just-saved key), so the
    // surface you are looking at re-renders in the new theme immediately. We deliberately do NOT rebuild the themed
    // HOME underlay here: showThemedHome() ends in stack_->setCurrentWidget(themedHome_), which would yank the
    // current page away from this panel — exactly the wedge follow-up #5 recorded against the classic panel
    // (Back could no longer leave the host). The FULL theme (home/browse/detail layout) applies on exit: the hub
    // root's onBack already calls showHomeScreen(), which rebuilds the themed home from the saved key. So:
    // apply-on-select restyles the panel live; the full theme lands on the way out — no underlay rebuild, no wedge.
    //
    // TOGGLE: turning themedHome/enabled OFF while inside the themed panel just SAVES the key (classic semantics —
    // value persists, takes effect on next navigation). We do NOT hot-swap the whole UI mid-panel; the panel stays
    // up until the user leaves, at which point normal navigation honours the new value (Back to the hub root ->
    // showHomeScreen renders the classic home if it was turned off). Least-surprising, and matches the classic setter.
    if (themedHomeEnabled() && themedPanelHost_)
    {
        // The theme row is now an ACTION that opens ThemePickerHost — the one chooser surface, with a REAL live
        // preview. It replaces the old Choice row (cycle display names in place) and its "applies live" Info row:
        // that "preview" only ever recoloured this panel, which is not what the theme looks like. Losing in-panel
        // cycling is the deliberate trade (roadmap #57).
        const QString curFolder = currentThemeFolder();

        themedPanelHost_->setStyle(settingsPanelStyle());   // the active theme's settingsPanel block (hard fallbacks)

        QVector<PanelRow> rows;
        auto sep    = [&rows](const QString& t) { PanelRow r; r.kind = PanelRow::Separator; r.label = t; rows << r; };
        auto info   = [&rows](const QString& id, const QString& label, const QString& value) {
            PanelRow r; r.kind = PanelRow::Info; r.id = id; r.label = label; r.value = value; rows << r; };
        auto toggle = [&rows](const QString& id, const QString& label, bool on) {
            PanelRow r; r.kind = PanelRow::Toggle; r.id = id; r.label = label; r.checked = on; rows << r; };
        auto action = [&rows](const QString& id, const QString& label) {
            PanelRow r; r.kind = PanelRow::Action; r.id = id; r.label = label; rows << r; };
        auto choice = [&rows](const QString& id, const QString& label, const QStringList& opts, const QString& cur) {
            PanelRow r; r.kind = PanelRow::Choice; r.id = id; r.label = label; r.options = opts; r.value = cur; rows << r; };

        // Display-mode <-> stored-value mapping (Choice cycles display names; the handler maps back to the stored
        // key). Order matches the stored values below one-for-one.
        const QStringList dispOpts   = { tr("Auto"), tr("Desktop"), tr("TV"), tr("Mobile") };
        const QStringList dispValues = { QStringLiteral("auto"), QStringLiteral("desktop"),
                                         QStringLiteral("tv"), QStringLiteral("mobile") };
        const int dispIdx = qMax(0, dispValues.indexOf(Settings::displayMode()));
        const QString dispCur = dispOpts.value(dispIdx);

        toggle(QStringLiteral("appr.themed"), tr("Use the themed home screen (beta)"), themedHomeEnabled());
        sep(tr("Theme"));
        // The CURRENT theme's own form-factor verdict, shown on the row that opens the picker (issue #32).
        // This is where a user who has already landed on a wrong-looking home actually looks, and without it
        // the note would only appear once they were inside the picker — after the confusion, not before it.
        {
            const QString curNote =
                ThemeFormFactors::shortNote(ThemeEngine::themeFormFactorFit(curFolder));
            const QString curLabel = curNote.isEmpty()
                                   ? ThemeEngine::themeDisplayName(curFolder)
                                   : tr("%1  (%2)").arg(ThemeEngine::themeDisplayName(curFolder), curNote);
            action(QStringLiteral("appr.theme"), tr("Theme…   %1").arg(curLabel));
        }
        sep(tr("Display mode"));
        choice(QStringLiteral("appr.dispmode"), tr("Display mode"), dispOpts, dispCur);
        info(QStringLiteral("appr.dispmodehint"),
             tr("Auto fits this device; TV enlarges text and controls for the couch, Mobile for touch."), QString());
        sep(tr("Get more themes"));
        info(QStringLiteral("appr.customise"),
             tr("Edit a theme's theme.json to customise it (colours, layout, artwork)."), QString());
        info(QStringLiteral("appr.root"), tr("Themes folder"), ThemeEngine::themesRoot());
        info(QStringLiteral("appr.community"),
             tr("Browse the registry below, or share your own at github.com/cubman3134/everythingbox-themes."),
             QString());
        action(QStringLiteral("appr.browse"), tr("Browse community themes…"));
        action(QStringLiteral("appr.decorations"), tr("Browse decoration packs…"));

        auto onAct =
            [this, dispOpts, dispValues](const QString& id, const QString& val) {
                if (id == QStringLiteral("appr.themed")) {
                    // Save only (classic semantics); do NOT hot-swap the UI mid-panel — see the note above.
                    store().setValue(QStringLiteral("themedHome/enabled"), val == QStringLiteral("1"));
                    store().sync();
                }
                else if (id == QStringLiteral("appr.dispmode")) {
                    // Map the picked display name back to its stored value, save, then re-resolve the form factor.
                    // FormFactor::changed then live-restyles the widget surfaces (applyFormFactorWidgets) and the
                    // QML form.* bindings update automatically; restyle THIS panel on the same setStyle mechanism
                    // as the theme row so the surface you are looking at re-renders immediately.
                    const int i = dispOpts.indexOf(val);
                    const QString mode = dispValues.value(i < 0 ? 0 : i, QStringLiteral("auto"));
                    Settings::setDisplayMode(mode);
                    FormFactor::instance().refresh();
                    themedPanelHost_->setStyle(settingsPanelStyle());
                }
                else if (id == QStringLiteral("appr.theme")) {
                    // The real preview lives in ThemePickerHost. This replaces the old "recolour this panel and
                    // call that the preview" approximation.
                    if (!themePickerHost_) return;   // consistency with every other themePickerHost_ call site
                    themePickerHost_->setStyle(settingsPanelStyle());
                    // REFUSAL (present() == false): no theme is installed, so there is nothing to offer and
                    // nothing to preview — nothing was shown and no callback will fire. STAY on Appearance (the
                    // page the user is already looking at, which is a live surface with real actions) and say why,
                    // instead of switching the stack to a surface that refused to present.
                    if (!themePickerHost_->present(
                            tr("Theme"), currentThemeFolder(), /*mustChoose*/ false,
                            [this](const QString& folder) {
                                // UNCONDITIONAL write. Appearance highlights the RESOLVED folder while nothing may
                                // be stored, so confirming the already-highlighted row MUST still commit — else
                                // needsPick stays true and the forced step re-prompts for the choice just made.
                                ThemeChoice::setForProfile(ProfileStore::currentId(), folder);
                                openAppearance();       // re-render Appearance with the new style + label
                            },
                            [this] { openAppearance(); }))
                    {
                        notify(tr("No themes found in %1.")
                                   .arg(QDir::toNativeSeparators(ThemeEngine::themesRoot())), kFeedbackLong);
                        return;
                    }
                    stack_->setCurrentWidget(themePickerHost_);
                    updateNavForPage();
                }
                else if (id == QStringLiteral("appr.decorations")) {
                    // The decoration (bezel) gallery, nested on this host exactly as the theme gallery is.
                    // Both live on Appearance because both are "make it look different" and both come out of
                    // the same registry index; the emulation panel carries the same verb for the same reason
                    // it carries the bezel toggle.
                    presentDecorationRegistry();
                }
                else if (id == QStringLiteral("appr.browse")) {
                    // The IN-APP gallery, the themed twin of the classic panel's RegistryBrowser(Themes). Nested
                    // on this host (present -> one more graph level), so Back lands back on Appearance and the
                    // theme installed here is in Theme…'s picker the moment that panel is rebuilt.
                    presentThemeRegistry();
                }
            };
        // defensive root onBack: Appearance is nested, so a pop re-renders the hub
        //
        // A THEME PREVIEW IS NOT ENDED HERE, and deliberately (issue #91). This lambda does not run on an
        // ordinary Back at all — a nested level pops by re-rendering its parent panel, and only the ROOT
        // panel's onBack is ever called — so ending a preview here would be a claim the code does not keep,
        // on exactly one of the two surfaces. A preview ends where BOTH surfaces leave: the settings area
        // itself (leaveSettingsArea), which covers Back out of the hub, Home, and the F8 shortcut alike.
        auto onBack = [this] { openSettingsHub(); };
        // REPLACE the top level when Appearance is already the top panel. openAppearance() is now RE-ENTRANT — the
        // picker's onPicked/onBack both call it to re-render the label and style — and a plain present() would push
        // a second "Appearance" level every round trip, so Back would walk back through stale copies of this panel
        // instead of reaching the hub. Same idiom as Emulators / Add-ons / Downloads.
        if (themedPanelHost_->panelTitle() == tr("Appearance"))
            themedPanelHost_->replaceTop(tr("Appearance"), rows, onAct, onBack);
        else
            themedPanelHost_->present(tr("Appearance"), rows, onAct, onBack);

        stack_->setCurrentWidget(themedPanelHost_);
        updateNavForPage();
        return;
    }

    if (stack_->currentWidget() != panelPage_) panelReturnTo_ = stack_->currentWidget();

    // A representative stand-in for the preview: the real categories when we have them, else the SHARED synthetic
    // set ThemePickerHost previews — one source, so the two previews cannot drift.
    // home_ is constructed unconditionally in the ctor, so it always EXISTS here even pre-home (the forced theme
    // step runs before openHome); what is empty before openHome is its category list, and that is exactly what the
    // synthetic fallback below covers.
    QVariantList previewItems = home_->categoryItems();
    if (previewItems.isEmpty()) previewItems = ThemePickerHost::previewItems();
    QVariantMap previewSystem; previewSystem.insert(QStringLiteral("name"), QStringLiteral("EverythingBox"));

    showPanel(tr("Appearance"), [this, previewItems, previewSystem](QVBoxLayout* v) {
        // A PREVIEW IS RUNNING (issue #91): say so, and say what leaving does. Without this line the panel
        // is indistinguishable from one where the user simply chose that theme — and the restore on the way
        // out would then look like the app undoing their choice. The preview box below already shows the
        // previewed theme, because it is the theme now.
        if (themePreviewActive())
        {
            auto* note = new QLabel(tr("Previewing \"%1\" — leaving Settings puts your own theme back. "
                                       "Pick it in the list to keep it.").arg(themePreviewFolder_));
            note->setWordWrap(true);
            note->setTextFormat(Qt::PlainText);   // a theme FOLDER name, which comes off a registry
            note->setStyleSheet(QStringLiteral("font-weight:bold;"));
            v->addWidget(note);
        }
        auto* enable = new QCheckBox(tr("Use the themed home screen (beta)"));
        enable->setChecked(themedHomeEnabled());
        connect(enable, &QCheckBox::toggled, this, [this](bool on) {
            store().setValue(QStringLiteral("themedHome/enabled"), on); store().sync();
        });
        v->addWidget(enable);

        auto* row = new QHBoxLayout();
        auto* leftCol = new QVBoxLayout();
        leftCol->addWidget(new QLabel(tr("Theme")));
        auto* list = new QListWidget();
        list->setMinimumWidth(240);
        list->setSpacing(4); // breathing room so rows don't crowd/overlap, especially the selected one
        list->setStyleSheet(QStringLiteral(
            "QListWidget{outline:none;border:none;}"
            "QListWidget::item{padding:10px 12px;border-radius:6px;}"
            "QListWidget::item:selected{background:#2D6CDF;color:white;}"));
        const QStringList themes = ThemeEngine::availableThemes();
        const QString current = currentThemeFolder();
        for (const QString& folder : themes)
        {
            // The CLASSIC twin of the themed picker's per-row form-factor note (issue #32). A user who never
            // turns the themed home on picks their theme here and nowhere else, so a marker that existed only
            // in ThemePicker.qml would be invisible to exactly the people most likely to be on an odd device.
            // Same wording (ThemeFormFactors::shortNote), same never-hide rule: every installed theme is
            // listed and every row is selectable — the note is the whole of the behaviour.
            const QString note = ThemeFormFactors::shortNote(ThemeEngine::themeFormFactorFit(folder));
            // The note goes on its OWN line, not appended to the name. This list is 240px wide beside a big
            // live preview, and "Night — EverythingBox — Not listed for this device" on one line is cut off
            // mid-word by the viewport: a truncated warning is worse than none, because the user can see
            // that something was said and not what. A newline makes the default delegate lay out two lines
            // and grow the row, exactly like the themed picker's.
            const QString label = note.isEmpty()
                                ? ThemeEngine::themeDisplayName(folder)
                                : (ThemeEngine::themeDisplayName(folder) + QLatin1Char('\n') + note);
            auto* it = new QListWidgetItem(label, list);
            // The note rides in the visible text but NEVER in the stored value: the folder is what gets
            // committed, and it is carried in UserRole exactly as before.
            it->setData(Qt::UserRole, folder);
            if (!note.isEmpty()) it->setToolTip(note);
            if (folder == current) list->setCurrentItem(it);
        }
        if (!list->currentItem() && list->count() > 0) list->setCurrentRow(0);
        leftCol->addWidget(list, 1);
        auto* hint = new QLabel(tr("Edit a theme's theme.json to customise — it previews here live."));
        hint->setWordWrap(true);
        QFont hf = hint->font(); hf.setPointSizeF(hf.pointSizeF() * 0.85); hint->setFont(hf);
        leftCol->addWidget(hint);
        row->addLayout(leftCol, 0);

        auto* previewBox = new QFrame();
        previewBox->setFrameShape(QFrame::StyledPanel);
        previewBox->setMinimumSize(480, 300);
        auto* pv = new QVBoxLayout(previewBox);
        pv->setContentsMargins(1, 1, 1, 1);
        row->addWidget(previewBox, 1);
        v->addLayout(row, 1);

        // Getting more themes: the gallery is the way in, the GitHub link is how you contribute one (and
        // the fallback for a registry the gallery can't install from). Before this, the hand-copy was the
        // ONLY documented route, which is not performable on a TV box with no browser and no file manager.
        auto* share = new QLabel(tr(
            "<b>Get more themes &amp; share yours.</b> "
            "Browse the community registry below, or at "
            "<a href=\"https://github.com/cubman3134/everythingbox-themes\">github.com/cubman3134/everythingbox-themes</a>. "
            "Themes live in <code>%1</code> — each is a folder with a <code>theme.json</code>, so you can also "
            "add one by dropping its folder in there. "
            "To <b>share</b> yours, add the folder under <code>themes2/</code> in that repo with an "
            "<code>index.json</code> entry and open a pull request (see <code>THEME_FORMAT.md</code> for the format).")
            .arg(ThemeEngine::themesRoot()));
        share->setTextFormat(Qt::RichText);
        share->setWordWrap(true);
        share->setOpenExternalLinks(true); // the GitHub link opens in the browser
        share->setStyleSheet(QStringLiteral("margin-top:12px;"));
        v->addWidget(share);

        // The in-app gallery. Hosted INLINE via showDialogPanel (it sets Qt::Widget), never as a top-level
        // window — this panel lives inside panelRing_ and a real dialog would be unreachable with a D-pad.
        // Same *inline* treatment (Qt::Widget) LibraryView::browseAddons uses for the add-on browser — that
        // one is hosted differently though (LibraryView::showDialogPage/pushPage gives it a whole page).
        auto* browse = new QPushButton(tr("Browse community themes…"));
        browse->setMinimumHeight(40);
        connect(browse, &QPushButton::clicked, this, [this] {
            auto* dlg = new RegistryBrowser(RegistryBrowser::Themes, nullptr, this);
            // PREVIEW (issue #91). The browser names the theme; applying one is MainWindow's — it owns the
            // per-profile choice, the re-render and the restore, and the same two calls end a preview
            // started on the themed gallery. The browser is then CLOSED, because the thing the user asked
            // to look at is rendered by the panel this dialog is sitting on top of: Appearance's live
            // preview box rebuilds from the just-written choice as soon as the panel is re-rendered, which
            // the finished handler below does unconditionally.
            dlg->setPreviewHandler([this, dlg](const QString& folder) {
                beginThemePreview(folder);
                dlg->closeWhenIdle();
            });
            showDialogPanel(tr("Browse Themes"), dlg, [this](int) {
                // Re-render Appearance EITHER WAY, install or not. availableThemes() reads the directory
                // live, so a newly installed theme is in the list as soon as the panel is rebuilt. And it
                // must be unconditional: the browser's own Close button only HIDES the inline dialog, so a
                // "did you install?" guard here would strand a user who just looked on an empty panel.
                openAppearance();
            }, [this, dlg] {
                // Back is live while an install is running, and an install is synchronous: it sits in a
                // nested event loop per file. Navigating now would delete the browser out from under those
                // frames (showPanel replaces the panel content), and the download code would return into
                // freed memory — reproduced as an access violation before this guard. Hand the exit to the
                // dialog instead; it leaves the moment the install finishes, through the handler above.
                if (dlg->isInstalling()) { dlg->closeWhenIdle(); return; }
                openAppearance();
            });
        });
        v->addWidget(browse);

        // The decoration (bezel) gallery, hosted the same way and for the same reasons — inline, never a
        // top-level window. Its twin on the themed surface is the "appr.decorations" row above; a gallery
        // that exists on only one of the two builders is a gallery half this product's users cannot reach.
        auto* browseDeco = new QPushButton(tr("Browse decoration packs…"));
        browseDeco->setMinimumHeight(40);
        connect(browseDeco, &QPushButton::clicked, this, [this] {
            auto* dlg = new RegistryBrowser(RegistryBrowser::Decorations, nullptr, this);
            showDialogPanel(tr("Browse Decorations"), dlg, [this](int) { openAppearance(); },
                            [this, dlg] {
                // Same mid-install exit guard as the theme gallery: a decoration install spins a nested
                // event loop for its download, and navigating away would delete the dialog under it.
                if (dlg->isInstalling()) { dlg->closeWhenIdle(); return; }
                openAppearance();
            });
        });
        v->addWidget(browseDeco);

        auto rebuildPreview = [this, pv, previewBox, previewItems, previewSystem](const QString& folder) {
            while (QLayoutItem* old = pv->takeAt(0)) { if (old->widget()) old->widget()->deleteLater(); delete old; }
            // buildPreview, not buildView: the preview must never take the D-pad cursor, and that is now a
            // property of construction (ThemeEngine.h) rather than a line each preview site has to remember.
            // This panel lives inside panelRing_, so a Qt::StrongFocus 480x300 view would join the ring with
            // no action and no focus outline, and arrowing into it reads as the selector vanishing (#40).
            // buildPreview also seeds `categories`, so an XMB theme shows its cross.
            QWidget* p = ThemeEngine::buildPreview(ThemeEngine::themesRoot() + QStringLiteral("/") + folder,
                                                   previewItems, previewSystem, previewBox);
            if (!p) return;
            p->setMinimumSize(480, 270);
            // NOT WA_TransparentForMouseEvents, unlike ThemePickerHost: this panel has no click-through
            // target behind the preview, so leaving mouse handling alone is correct here.
            pv->addWidget(p);
        };
        // The commit path. The list highlights the RESOLVED folder (currentThemeFolder()) while nothing may
        // actually be stored, and currentItemChanged only fires on a CHANGE: a user who opened this panel and
        // clicked the already-selected theme would otherwise write nothing, needsPick would stay true, and the
        // forced first-run step would re-prompt them for the choice they just made. Hence itemClicked/itemActivated
        // are wired too — those DO fire on the current row.
        //
        // ONE USER ACTION IS ONE WRITE. Those three signals overlap: a single click emits currentItemChanged AND
        // itemClicked (and a double-click emits itemClicked twice plus itemActivated), so an unconditional write
        // stored the same key two or three times per click. `written` is the dedupe key: it starts EMPTY (opening
        // the panel is not a commit), so the first interaction always writes — including a click on the
        // already-highlighted row, which is the case the extra signals exist for. Re-picking a row after visiting
        // another one writes again, because the key moved. Deduping on the WRITTEN folder, not on a "has written"
        // flag, is what keeps that true.
        auto shown = std::make_shared<QString>();
        auto written = std::make_shared<QString>();
        auto commit = [this, rebuildPreview, shown, written](QListWidgetItem* it) {
            if (!it) return;
            const QString folder = it->data(Qt::UserRole).toString();
            if (folder.isEmpty()) return;
            if (*written != folder)
            { *written = folder; ThemeChoice::setForProfile(ProfileStore::currentId(), folder); }
            if (*shown != folder) { *shown = folder; rebuildPreview(folder); }
        };
        connect(list, &QListWidget::currentItemChanged, this,
                [commit](QListWidgetItem* it, QListWidgetItem*) { commit(it); });
        connect(list, &QListWidget::itemClicked,   this, commit);
        connect(list, &QListWidget::itemActivated, this, commit);
        // Opening the panel is NOT a commit: it must not WRITE the resolved folder. That value syncs across
        // devices (ThemeChoice::needsPick's note), so persisting a per-device resolution here would overwrite the
        // theme the user chose on another device. Seed the preview only.
        if (QListWidgetItem* it = list->currentItem())
        { *shown = it->data(Qt::UserRole).toString(); rebuildPreview(*shown); }
    // No endThemePreview() here either — see the themed builder. A preview ends when the SETTINGS AREA is
    // left (leaveSettingsArea), which is the one edge both surfaces genuinely share; ending it one level
    // earlier here would give the classic layout a shorter preview than the themed one for no reason the
    // user could see.
    }, [this] { openSettingsHub(); });
}
#else
void MainWindow::openAppearance() {}
#endif

// The subtitle button opens a full-player overlay panel (Stremio-style) rather than a dropdown: a dimmed
// scrim over the whole video with a centred card holding the track picker, sync/size adjusters, and the
// load/download sources. Clicking the scrim, the ✕, or pressing Esc/Back closes it; it's arrow-navigable.
// A signed millisecond readout for a sync offset (seconds): "+150 ms", "0 ms", "−100 ms" (U+2212 minus, so it
// matches the − glyph on the step buttons). The label shows this re-read from mpv, never our own arithmetic.
static QString formatMs(double secs)
{
    const long ms = qRound(secs * 1000.0);
    if (ms == 0) return QStringLiteral("0 ms");
    const QChar sign = ms > 0 ? QLatin1Char('+') : QChar(0x2212);
    return QStringLiteral("%1%2 ms").arg(sign).arg(qAbs(ms));
}

void MainWindow::showSubtitleMenu()
{
    if (subOverlay_) { hideSubtitleMenu(); return; } // second press on the button toggles it closed
    revealMediaControls();

    subOverlay_ = new QWidget(player_);
    subOverlay_->setObjectName(QStringLiteral("subScrim"));
    subOverlay_->setStyleSheet(QStringLiteral("#subScrim { background: rgba(8,8,12,0.60); }"));
    // Give the overlay its own arrow cursor so it never inherits the player's blanked cursor (which the
    // inactivity timer sets in full screen) over the scrim/card background.
    subOverlay_->setCursor(Qt::ArrowCursor);
    subOverlay_->setGeometry(player_->rect());
    subOverlay_->installEventFilter(this); // a click on the scrim (outside the card) dismisses

    // Centre the card within the scrim.
    auto* centre = new QVBoxLayout(subOverlay_);
    centre->setContentsMargins(0, 0, 0, 0);
    centre->addStretch(1);
    auto* midRow = new QHBoxLayout();
    midRow->addStretch(1);

    auto* card = new QFrame(subOverlay_);
    card->setObjectName(QStringLiteral("subCard"));
    card->setStyleSheet(QStringLiteral(
        "#subCard { background:#16161c; border:1px solid rgba(255,255,255,0.14); border-radius:16px; }"
        "#subCard QLabel { color:#e8e8e8; }"));
    const int cardW = qBound(360, player_->width() - 60, 760);
    card->setFixedWidth(cardW);
    card->setMaximumHeight(qMax(300, player_->height() - 48));
    midRow->addWidget(card);
    midRow->addStretch(1);
    centre->addLayout(midRow);
    centre->addStretch(1);

    auto* cv = new QVBoxLayout(card);
    cv->setContentsMargins(24, 20, 24, 20);
    cv->setSpacing(14);

    // Header: title + close.
    auto* headRow = new QHBoxLayout();
    auto* title = new QLabel(tr("Audio & Subtitles"), card);
    title->setStyleSheet(QStringLiteral("font-size:20px;font-weight:bold;"));
    headRow->addWidget(title, 1);
    // The × is DRAWN, not typed: the themed UI font need not carry U+00D7 (or any other cross), and when it
    // doesn't the button renders completely empty — which is exactly how this one shipped. Two painted strokes
    // look the same in every font and on every platform.
    auto* closeBtn = new QPushButton(card);
    {
        QPixmap px(20, 20);
        px.setDevicePixelRatio(devicePixelRatioF());
        px.fill(Qt::transparent);
        QPainter pp(&px);
        pp.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(0xE8, 0xE8, 0xE8));
        pen.setWidthF(2.2);
        pen.setCapStyle(Qt::RoundCap);
        pp.setPen(pen);
        pp.drawLine(6, 6, 14, 14);
        pp.drawLine(14, 6, 6, 14);
        pp.end();
        closeBtn->setIcon(QIcon(px));
        closeBtn->setIconSize(QSize(20, 20));
    }
    closeBtn->setToolTip(tr("Close"));
    closeBtn->setAccessibleName(tr("Close"));
    closeBtn->setFixedSize(32, 32);
    closeBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background:rgba(255,255,255,0.08); color:#e8e8e8; border:none; border-radius:8px;"
        " font-size:22px; font-weight:bold; padding-bottom:3px; }"
        "QPushButton:hover, QPushButton:focus { background:rgba(90,140,255,0.75); }"));
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, this, [this] { hideSubtitleMenu(); });
    headRow->addWidget(closeBtn);
    cv->addLayout(headRow);

    // Two columns side by side: the track list (left) and the sync/size/source controls (right), so you can
    // reach the settings directly instead of walking the whole track list.
    auto* body = new QHBoxLayout();
    body->setSpacing(22);
    auto* leftCol = new QVBoxLayout();
    leftCol->setSpacing(8);
    auto* rightCol = new QVBoxLayout();
    rightCol->setSpacing(10);

    auto sectionLabel = [card](const QString& t) {
        auto* l = new QLabel(t, card);
        l->setStyleSheet(QStringLiteral("font-size:13px;font-weight:bold;color:#9aa0aa;"));
        return l;
    };

    // Flat full-width row button used for tracks + source actions; `on` gives it the accent selected look.
    auto rowButton = [card](const QString& text, bool on) {
        auto* b = new QPushButton(text, card);
        b->setMinimumHeight(38);
        b->setCursor(Qt::PointingHandCursor);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setStyleSheet(QString(QStringLiteral(
            "QPushButton { text-align:left; padding:6px 14px; border-radius:8px; color:#e8e8e8; font-size:15px;"
            " background:%1; border:1px solid %2; }"
            "QPushButton:hover, QPushButton:focus { background:rgba(90,140,255,0.55); border:1px solid rgba(255,255,255,0.5); }"))
            .arg(on ? QStringLiteral("rgba(90,140,255,0.30)") : QStringLiteral("rgba(255,255,255,0.05)"),
                 on ? QStringLiteral("rgba(90,140,255,0.9)")  : QStringLiteral("transparent")));
        return b;
    };

    // --- LEFT: audio-track picker + subtitle-track picker, scrollable so long lists stay contained. ---
    auto* trackScroll = new QScrollArea(card);
    trackScroll->setWidgetResizable(true);
    trackScroll->setFrameShape(QFrame::NoFrame);
    trackScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    trackScroll->setStyleSheet(QStringLiteral("background:transparent;"));
    trackScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    subTrackScroll_ = trackScroll;
    trackScroll->installEventFilter(this); // see eventFilter: it would otherwise eat the panel's arrow keys
    auto* trackHost = new QWidget(trackScroll);
    auto* tv = new QVBoxLayout(trackHost);
    tv->setContentsMargins(0, 0, 6, 0);
    tv->setSpacing(6);
    subLeftCol_ = {};
    QPushButton* initial = nullptr;

    // A track's display label: "LANG · Title", or a numbered fallback.
    auto trackLabel = [this](const MpvWidget::Track& t) {
        const QString lang = t.lang.isEmpty() ? QString() : t.lang.toUpper();
        QString label = lang;
        if (!t.title.isEmpty()) label = label.isEmpty() ? t.title : lang + QStringLiteral("  ·  ") + t.title;
        return label.isEmpty() ? tr("Track %1").arg(t.id) : label;
    };

    // Audio tracks (only when the file actually has some to choose between / label).
    const auto audio = player_->audioTracks();
    if (!audio.isEmpty())
    {
        tv->addWidget(sectionLabel(tr("AUDIO")));
        for (const MpvWidget::Track& t : audio)
        {
            auto* b = rowButton((t.selected ? QStringLiteral("✓  ") : QStringLiteral("     ")) + trackLabel(t), t.selected);
            const int id = t.id;
            connect(b, &QPushButton::clicked, this, [this, id] { player_->setAudioTrack(id); hideSubtitleMenu(); });
            tv->addWidget(b);
            subLeftCol_ << b;
            if (t.selected && !initial) initial = b;
        }
        tv->addSpacing(8);
    }

    // Subtitle tracks (Off + each).
    tv->addWidget(sectionLabel(tr("SUBTITLE")));
    const auto tracks = player_->subtitleTracks();
    bool anySelected = false;
    for (const MpvWidget::Track& t : tracks) if (t.selected) anySelected = true;

    auto* offBtn = rowButton(tr("Off"), !anySelected);
    connect(offBtn, &QPushButton::clicked, this, [this] { player_->setSubtitleTrack(-1); hideSubtitleMenu(); });
    tv->addWidget(offBtn);
    subLeftCol_ << offBtn;
    if (!initial) initial = offBtn;
    for (const MpvWidget::Track& t : tracks)
    {
        auto* b = rowButton((t.selected ? QStringLiteral("✓  ") : QStringLiteral("     ")) + trackLabel(t), t.selected);
        const int id = t.id;
        connect(b, &QPushButton::clicked, this, [this, id] { player_->setSubtitleTrack(id); hideSubtitleMenu(); });
        tv->addWidget(b);
        subLeftCol_ << b;
        if (t.selected) initial = b;
    }
    if (tracks.isEmpty())
    {
        auto* none = new QLabel(tr("No subtitle tracks — load or download one on the right."), card);
        none->setStyleSheet(QStringLiteral("color:#999;font-size:13px;padding:2px 4px;"));
        none->setWordWrap(true);
        tv->addWidget(none);
    }
    tv->addStretch(1);
    trackScroll->setWidget(trackHost);
    leftCol->addWidget(trackScroll, 1);

    // --- RIGHT: sync + size adjusters (−/+ update live), then the source actions. ---
    // Close is the first right-column focus target (it sits top-right), so Up from the settings reaches it.
    subRightCol_ = { closeBtn };
    rightCol->addWidget(sectionLabel(tr("SUBTITLE SIZE")));
    auto addAdjustRow = [this, card, rightCol](const QString& name, std::function<QString()> value,
                                               std::function<void()> minus, std::function<void()> plus) {
        auto* w = new QWidget(card);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(10);
        auto* lbl = new QLabel(name + QStringLiteral(":  ") + value(), w);
        lbl->setStyleSheet(QStringLiteral("font-size:15px;"));
        h->addWidget(lbl, 1);
        auto mkBtn = [w](const QString& t) {
            auto* b = new QPushButton(t, w);
            b->setFixedSize(40, 34);
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QStringLiteral(
                "QPushButton { background:rgba(255,255,255,0.10); color:#e8e8e8; border:none; border-radius:8px;"
                " font-size:19px;font-weight:bold; padding:0; }"
                "QPushButton:hover, QPushButton:focus { background:rgba(90,140,255,0.75); }"));
            return b;
        };
        auto* minusBtn = mkBtn(QString(QChar(0x2212))); // − minus sign
        auto* plusBtn = mkBtn(QStringLiteral("+"));
        h->addWidget(minusBtn);
        h->addWidget(plusBtn);
        connect(minusBtn, &QPushButton::clicked, w, [=] { minus(); lbl->setText(name + QStringLiteral(":  ") + value()); });
        connect(plusBtn,  &QPushButton::clicked, w, [=] { plus();  lbl->setText(name + QStringLiteral(":  ") + value()); });
        rightCol->addWidget(w);
        subRightCol_ << minusBtn << plusBtn;
    };
    addAdjustRow(tr("Size"),
                 [this] { return QStringLiteral("%1%").arg(qRound(player_->subtitleScale() * 100)); },
                 [this] { player_->setSubtitleScale(qMax(0.2, player_->subtitleScale() - 0.1)); },
                 [this] { player_->setSubtitleScale(qMin(4.0, player_->subtitleScale() + 0.1)); });

    // AUDIO SYNC + SUBTITLE SYNC: one lambda builds both sections (DRY), parameterized on the axis + its live
    // getter/setter. Compact so the whole card fits at TV scale without a (focus-trapping) scroll area: a readout
    // + inline ∓50 ms steppers on one row (like the Size row above), then a Reset / Save-as-default row. The
    // readout re-reads mpv after every change (shows mpv truth, never our arithmetic). Per-file steps persist via
    // SyncOffsets keyed by syncKey_; Reset drops the per-file entry (revert to the global default); Save-as-default
    // promotes the current value to the global. Every focusable button joins subRightCol_ + is hitClamp-sized.
    auto addSyncSection = [this, card, rightCol, rowButton, sectionLabel](const QString& heading, SyncOffsets::Which w,
                              std::function<double()> getter, std::function<void(double)> setter) {
        FormFactor& ff = FormFactor::instance();
        rightCol->addSpacing(6);
        rightCol->addWidget(sectionLabel(heading));

        // Row 1: live readout (mpv truth) + the ∓50 ms steppers, on one line.
        auto* r1 = new QWidget(card);
        auto* h1 = new QHBoxLayout(r1);
        h1->setContentsMargins(0, 0, 0, 0);
        h1->setSpacing(10);
        auto* readout = new QLabel(formatMs(getter()), r1);
        readout->setStyleSheet(QStringLiteral("font-size:15px;font-weight:bold;"));
        h1->addWidget(readout, 1);
        auto mkStep = [r1, &ff](const QString& t) {
            auto* b = new QPushButton(t, r1);
            b->setMinimumSize(ff.hitClamp(58), ff.hitClamp(34));
            b->setAutoRepeat(true);                 // hold to keep nudging
            b->setCursor(Qt::PointingHandCursor);
            b->setStyleSheet(QStringLiteral(
                "QPushButton { background:rgba(255,255,255,0.10); color:#e8e8e8; border:none; border-radius:8px;"
                " font-size:14px;font-weight:bold; padding:0 6px; }"
                "QPushButton:hover, QPushButton:focus { background:rgba(90,140,255,0.75); }"));
            return b;
        };
        auto* minusBtn = mkStep(QString(QChar(0x2212)) + QStringLiteral("50"));
        auto* plusBtn  = mkStep(QStringLiteral("+50"));
        h1->addWidget(minusBtn);
        h1->addWidget(plusBtn);
        rightCol->addWidget(r1);

        auto step = [this, w, getter, setter, readout](double delta) {
            const double v = qBound(-10.0, getter() + delta, 10.0);
            setter(v);
            SyncOffsets::savePerFile(syncKey_, w, v);
            readout->setText(formatMs(getter())); // re-read from mpv
        };
        connect(minusBtn, &QPushButton::clicked, this, [step] { step(-0.05); });
        connect(plusBtn,  &QPushButton::clicked, this, [step] { step(+0.05); });
        subRightCol_ << minusBtn << plusBtn;

        // Row 2: Reset (revert to global) + Save as default (promote current to global), side by side.
        auto* resetBtn = rowButton(tr("Reset"), false);
        resetBtn->setMinimumHeight(ff.hitClamp(34));
        connect(resetBtn, &QPushButton::clicked, this, [this, w, setter, readout] {
            SyncOffsets::clearPerFile(syncKey_, w);
            setter(SyncOffsets::globalDefault(w));
            readout->setText(formatMs(w == SyncOffsets::Which::Audio ? player_->audioDelay()
                                                                     : player_->subtitleDelay()));
        });
        auto* saveBtn = rowButton(tr("Save as default"), false);
        saveBtn->setMinimumHeight(ff.hitClamp(34));
        connect(saveBtn, &QPushButton::clicked, this, [this, w, getter] {
            SyncOffsets::setGlobalDefault(w, getter());
            notify(tr("Saved as the default sync offset."), 2500);
        });
        auto* r2 = new QWidget(card);
        auto* h2 = new QHBoxLayout(r2);
        h2->setContentsMargins(0, 0, 0, 0);
        h2->setSpacing(10);
        h2->addWidget(resetBtn);
        h2->addWidget(saveBtn);
        rightCol->addWidget(r2);
        subRightCol_ << resetBtn << saveBtn;
    };
    addSyncSection(tr("AUDIO SYNC"), SyncOffsets::Which::Audio,
                   [this] { return player_->audioDelay(); },
                   [this](double v) { player_->setAudioDelay(v); });
    addSyncSection(tr("SUBTITLE SYNC"), SyncOffsets::Which::Sub,
                   [this] { return player_->subtitleDelay(); },
                   [this](double v) { player_->setSubtitleDelay(v); });

    rightCol->addSpacing(6);
    rightCol->addWidget(sectionLabel(tr("SOURCE")));
    auto* loadBtn = rowButton(tr("📂  Load from file…"), false);
    connect(loadBtn, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, tr("Load subtitle"), QString(),
            tr("Subtitles (*.srt *.ass *.ssa *.sub *.vtt *.idx);;All files (*)"));
        if (!f.isEmpty()) player_->addSubtitle(f);
        hideSubtitleMenu();
    });
    rightCol->addWidget(loadBtn);
    subRightCol_ << loadBtn;
    // #81: an API key (the user's, or the one built into this release) is all SEARCHING needs, so both rows
    // appear on that alone; a download without a stored login asks for one first (MainWindowSubtitleLogin.cpp).
    if (SubtitleFetcher::canSearch() && !(subCtx_.imdbStreamId.isEmpty() && subCtx_.title.isEmpty()))
    {
        auto* dlBtn = rowButton(tr("🔍  Download from OpenSubtitles"), false);
        connect(dlBtn, &QPushButton::clicked, this, [this] {
            hideSubtitleMenu();
            auto run = [this] {
                notify(tr("Searching OpenSubtitles for a subtitle…"), 0);
                subFetcher_->fetch(subCtx_.imdbStreamId, subCtx_.title, Settings::subtitleLanguage(),
                                   [this](const QString& srt) {
                    if (!srt.isEmpty()) { player_->addSubtitle(srt); notify(tr("Subtitle added."), 3000); }
                    else notify(tr("No matching subtitle found on OpenSubtitles."), kFeedbackLong);
                });
            };
            // This row downloads straight away, so without a login it asks for one before it starts.
            if (SubtitleFetcher::canDownload()) run();
            else promptOpenSubtitlesLogin(false, run);
        });
        rightCol->addWidget(dlBtn);
        subRightCol_ << dlBtn;

        // …and the MANUAL picker beside it: when the auto-pick grabbed the wrong rip (out-of-sync, a different
        // cut), this lists every row the best-matching tier returned so the user chooses the right one.
        auto* pickBtn = rowButton(tr("🔎  Search subtitles…"), false);
        connect(pickBtn, &QPushButton::clicked, this, [this] {
            hideSubtitleMenu();
            notify(tr("Searching OpenSubtitles…"), 0);   // sticky: replaced by the result / the picker
            const QString lang = Settings::subtitleLanguage();
            // Compose the cache key HERE — the one moment subCtx_ is definitionally the video the user is
            // asking about. Search + download are two network round-trips, and subCtx_ is rewritten by every
            // media open (armSubtitleFetch / openVideoPath / link / audio); a callback that read the live
            // member would file this .srt under whatever video happens to be open when the reply lands, and
            // the cache short-circuits before the network, so that mis-file would replay forever. Same shape
            // as the auto-fetch path above: key first, captured by value.
            const QString ident = SubtitleFetcher::cacheIdentifier(subCtx_.imdbStreamId, subCtx_.title,
                                                                   subCtx_.localPath);
            const QString key = SubtitleCache::keyFor(ident, lang);
            subFetcher_->searchList(subCtx_.imdbStreamId, subCtx_.title, lang, subCtx_.localPath,
                                    [this, lang, key](const QVector<SubtitleCandidate>& list) {
                if (list.isEmpty()) { notify(tr("No subtitles found on OpenSubtitles."), kFeedbackLong); return; }
                presentSubtitleCandidates(list, lang, key);
            });
        });
        rightCol->addWidget(pickBtn);
        subRightCol_ << pickBtn;
    }
    else if (!SubtitleFetcher::canSearch())
    {
        // Unconfigured: the whole OpenSubtitles block above is absent, so the feature would be INVISIBLE —
        // nothing tells you it exists or what it wants. A non-interactive hint row says both. It uses the
        // panel's existing info-label primitive (the same styling as the "No subtitle tracks…" line on the
        // left) rather than a disabled rowButton: a disabled QPushButton can't take focus, so parking one in
        // subRightCol_ would put a dead stop in the arrow ring. A QLabel is never in the ring by construction.
        // This arm is now guarded on `configured()` alone: the composite guard above ALSO fails for a
        // perfectly configured user opening a plain local file (openVideoPath clears subCtx_, so there is no
        // title/IMDB id), and telling them to add credentials they already have is wrong advice.
        auto* hint = new QLabel(tr("🔎  Search subtitles… (add an OpenSubtitles API key in Settings)"), card);
        hint->setStyleSheet(QStringLiteral("color:#999;font-size:13px;padding:2px 4px;"));
        hint->setWordWrap(true);
        rightCol->addWidget(hint);
    }
    else
    {
        // Configured, but this video carries no title/IMDB id to match on (a plain file opened straight from
        // disk). Neither button can do anything useful, so say WHY in the same info-label idiom rather than
        // showing credential advice that doesn't apply — and "Load from file…" above is still the live route.
        auto* hint = new QLabel(tr("🔎  Search subtitles… (this video has no title or IMDB id to match on)"), card);
        hint->setStyleSheet(QStringLiteral("color:#999;font-size:13px;padding:2px 4px;"));
        hint->setWordWrap(true);
        rightCol->addWidget(hint);
    }
    // The add-on subtitle tier (#79): shown whenever an enabled Stremio add-on offers `subtitles` for this
    // type and the video carries a Stremio-addressable id. It sits beside the OpenSubtitles rows in the same
    // SOURCE column, so a user with a subtitle add-on installed reaches it here regardless of whether
    // OpenSubtitles is configured — for the unconfigured majority it is the ONLY network subtitle source.
    if (addons_ && !subCtx_.imdbStreamId.isEmpty() && addons_->hasSubtitleProvider(subCtx_.type))
    {
        auto* addonBtn = rowButton(tr("🧩  Add-on subtitles…"), false);
        connect(addonBtn, &QPushButton::clicked, this, [this] {
            hideSubtitleMenu();
            notify(tr("Searching add-on subtitles…"), 0); // sticky: replaced by the picker / the result
            const QString lang = Settings::subtitleLanguage();
            // Pin the cache key HERE, the one moment subCtx_ is definitionally the video being asked about —
            // exactly as the OpenSubtitles picker does: subCtx_ is rewritten by every media open, and a reply
            // that lands after the user moved on must file its .srt under THIS video, not the next one.
            const QString ident = SubtitleFetcher::cacheIdentifier(subCtx_.imdbStreamId, subCtx_.title,
                                                                   subCtx_.localPath);
            const QString key = SubtitleCache::keyFor(ident, lang);
            addons_->listStremioSubtitles(subCtx_.type, subCtx_.imdbStreamId, subCtx_.localPath,
                                          [this, lang, key](const QVector<StremioTranslate::SubtitleAddonResult>& list) {
                if (list.isEmpty()) { notify(tr("No subtitles found from add-ons."), kFeedbackLong); return; }
                presentAddonSubtitles(list, lang, key);
            });
        });
        rightCol->addWidget(addonBtn);
        subRightCol_ << addonBtn;
    }
    rightCol->addStretch(1);

    body->addLayout(leftCol, 3);
    body->addLayout(rightCol, 2);
    cv->addLayout(body, 1);

    // Claim the arrow keys from every focusable control in the card (see eventFilter): a button that keeps
    // them walks Qt's tab chain instead, which is what made the sync column and the close button unreachable.
    for (QPushButton* b : subLeftCol_)  if (b) b->installEventFilter(this);
    for (QPushButton* b : subRightCol_) if (b) b->installEventFilter(this);

    subOverlay_->show();
    subOverlay_->raise();
    initial->setFocus(Qt::TabFocusReason); // land on the current track (or Off)
}
