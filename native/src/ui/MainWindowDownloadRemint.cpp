// AN ADD-ON DOWNLOAD RE-MINTS ITS LINK INSTEAD OF STORING IT (issue #437).
//
// downloads/queue.json used to keep a plain-link download's url, query and all, so an interrupted download
// could resume, and for a debrid or add-on stream link that query IS the credential. Jellyfin, Subsonic and
// Audiobookshelf downloads already stored an id and minted a link per request (#110/#193/#197). This file
// gives every add-on download whose resolve left a #224 re-mint recipe the same property: the job stores
// the recipe (core/DownloadRecipe.h), and this minter turns it back into a fresh link when the job next
// starts — after a restart, a pause, or a retry.
//
// NOT A SECOND RE-MINT SYSTEM. The routing is RecentStore::reopenFor's, and the two resolve calls are the
// ones remintAndOpen makes for a Continue Watching row. What differs is only the sink: a link handed to
// DownloadManager instead of a player, with no toast and no player chrome, because a download resuming in
// the background has nobody watching it. What the recipe cannot produce it says so with an empty answer,
// and the manager fails the job with a sentence that offers Retry (the recipe is intact).
//
// NEVER LOGGED: the minted url. The one line written per mint names the route, the type and the addon.
#include "MainWindow.h"

#include "../addons/AddonManager.h"
#include "../core/AppPaths.h"
#include "../core/DownloadManager.h"
#include "../core/DownloadRecipe.h"
#include "../core/RecentStore.h"

#include <QDateTime>
#include <QFile>
#include <QPointer>

#include <memory>

namespace {

// One line to <app>/stream_debug.log, beside the manager's own dlLog lines. Named `…Log(` so the log-discipline
// gate that reads log calls by that shape reads this one too.
void remintLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                 + QChar(QLatin1Char('\n'))).toUtf8());
}

} // namespace

void MainWindow::initDownloadRemint()
{
    if (!dm_) return;
    QPointer<MainWindow> self(this);
    dm_->setAsyncUrlMinter([self](const QString& sourceRef, DownloadManager::MintDone done) {
        DownloadRecipe::Recipe r;
        if (!self || !self->addons_ || !DownloadRecipe::decode(sourceRef, &r)) { done(QString(), {}); return; }

        // The recipe as the RECENTS row it was built as, so reopenFor routes it exactly as a Continue Watching
        // re-open would be routed — one table, pinned by probe_core, not a second copy of it here.
        RecentItem row;
        row.sourceRoute   = r.route;
        row.sourceType    = r.type;
        row.sourceAddonId = r.addonId;
        row.sourceItemId  = r.itemId;
        LoadedAddon* src = self->addons_->sourceById(r.addonId);
        const RecentStore::Reopen how = RecentStore::reopenFor(row, src != nullptr);

        // Exactly once, whatever the resolve does; a shared flag because AddonManager's callback is a copyable
        // std::function and an answer arriving twice must not start the transfer twice.
        auto answered = std::make_shared<bool>(false);
        auto finish = [done, answered, r](const QString& url, const QString&,
                                          const StreamHeaders::Headers& headers) {
            if (*answered) return;
            *answered = true;
            remintLog(QStringLiteral("download: re-mint via %1 (%2, %3) — %4")
                          .arg(r.route, r.type, r.addonId.isEmpty() ? QStringLiteral("-") : r.addonId,
                               url.isEmpty() ? QStringLiteral("no link") : QStringLiteral("fresh link")));
            done(url, headers);
        };

        switch (how)
        {
        case RecentStore::Reopen::ResolveImdb:
            // As the download crawl's imdb bridge asked it: across every stream provider, no preferred group.
            self->addons_->resolveStreamByImdb(r.type, r.itemId, finish);
            return;
        case RecentStore::Reopen::ResolveDirect:
        {
            MediaItem item;
            item.id   = r.itemId;
            item.type = r.type;
            self->addons_->resolveStream(src, item, finish);
            return;
        }
        case RecentStore::Reopen::SourceMissing:
        case RecentStore::Reopen::ReplayPath:
            break;
        }
        finish(QString(), QString(), {});   // the add-on is gone, or the recipe is not one a resolve can use
    });
}
