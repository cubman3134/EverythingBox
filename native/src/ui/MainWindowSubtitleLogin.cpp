// OpenSubtitles without a login until one is needed (#81): the MainWindow members that turn a download's
// "needs a login" into a sign-in prompt, and then finish the download the user asked for.
//
// Searching needs only an API key (the user's, else the one built into this release), so the picker lists
// results for anybody. DOWNLOADING needs the user's own OpenSubtitles account. The first download without one
// asks for the username and password through the nav kit's on-screen keyboard, stores them exactly where the
// Settings rows keep them (subs/osUser, subs/osPass, device-local), and carries on with the same row. The
// prompt belongs to the player's subtitle picker, which is the same surface on the themed and the classic
// layout, so there is one prompt and it serves both.
//
// NO NESTED LOOP. Osk::getText would spin one, and these prompts are raised from inside a NavMenu choice and a
// network reply's callback: the #28 / #211 family. The prompts are the non-blocking Osk (a callback when it
// closes), each raised a turn later off a single-shot timer, so nothing here runs inside the delivery that
// asked for it.
//
// NEVER LOGGED. The username and password go from the keyboard into Settings and nowhere else: no log line,
// no notice and no status text here mentions either one.
#include "MainWindow.h"

#include <QLineEdit>
#include <QPointer>
#include <QTimer>

#include "FeedbackPolicy.h"
#include "Notifier.h"
#include "../core/Settings.h"
#include "../core/SubtitleCache.h"
#include "../core/SubtitleFetcher.h"
#include "../video/MpvWidget.h"
#include "nav/Osk.h"

void MainWindow::promptOpenSubtitlesLogin(bool refused, std::function<void()> then)
{
    if (notifier_) notifier_->hideNotice();   // the sticky "Downloading subtitle…" would sit under the keyboard
    QPointer<MainWindow> self(this);
    const QString title = refused
        ? tr("OpenSubtitles didn't accept that login. Username")
        : tr("Downloading needs your OpenSubtitles account. Username");
    const QString initialUser = Settings::openSubUsername();
    QTimer::singleShot(0, this, [self, title, initialUser, then] {
        if (!self) return;
        new Osk(title, initialUser, QLineEdit::Normal, [self, then](const QString& user, bool ok) {
            if (!self) return;
            const QString u = user.trimmed();
            if (!ok || u.isEmpty())
            {
                self->notify(tr("No subtitle downloaded: OpenSubtitles needs your account for that."), kFeedbackLong);
                return;
            }
            QTimer::singleShot(0, self, [self, u, then] {
                if (!self) return;
                new Osk(tr("OpenSubtitles password"), QString(), QLineEdit::Password,
                        [self, u, then](const QString& pass, bool ok2) {
                    if (!self) return;
                    if (!ok2 || pass.isEmpty())
                    {
                        self->notify(tr("No subtitle downloaded: OpenSubtitles needs your account for that."),
                                     kFeedbackLong);
                        return;
                    }
                    // Where the Settings rows keep them, so the rows show the account and Discard/Save there
                    // behave as for anything typed in Settings. Stored, then the download goes ahead.
                    Settings::setOpenSubUsername(u);
                    Settings::setOpenSubPassword(pass);
                    if (then) then();
                }, self);
            });
        }, self);
    });
}

void MainWindow::downloadChosenSubtitle(qint64 fileId, const QString& lang, const QString& cacheKey,
                                        bool afterLogin)
{
    notify(tr("Downloading subtitle…"), 0);                        // sticky until the result lands
    QPointer<MainWindow> self(this);
    subFetcher_->downloadChoice(fileId, lang,
        [self, fileId, lang, cacheKey, afterLogin](const QString& srt, SubtitleFetcher::DownloadResult result) {
        if (!self) return;
        using R = SubtitleFetcher::DownloadResult;
        if (result == R::NeedsLogin || (result == R::LoginRefused && !afterLogin))
        {
            // Ask once, then retry this same row. A login refused straight after the user typed it is asked
            // again (they may have mistyped); a second refusal in a row ends with a notice instead of a loop.
            self->promptOpenSubtitlesLogin(result == R::LoginRefused, [self, fileId, lang, cacheKey] {
                if (self) self->downloadChosenSubtitle(fileId, lang, cacheKey, /*afterLogin=*/true);
            });
            return;
        }
        if (result == R::LoginRefused)
        {
            self->notify(tr("OpenSubtitles didn't accept that login. Check it in Settings."), kFeedbackLong);
            return;
        }
        if (srt.isEmpty()) { self->notify(tr("Couldn't download that subtitle."), kFeedbackLong); return; }
        // Overwrite the cache entry for this (identifier, language): the user's correction WINS over
        // whatever the auto-pick chose, and it sticks on every replay of this video. The key was PINNED
        // when the search was requested — subCtx_ is deliberately not read here, because a download that
        // lands after the user backed out and opened something else would otherwise file this .srt under
        // the new video's key (or the degenerate "title:" of a cleared context) and poison it for good.
        if (self->subCache_) self->subCache_->put(cacheKey, srt);
        // Attaching is a separate question from caching. The download is valid for the video it was
        // requested for, so it is always cached; but pasting it onto whatever is playing NOW would be
        // wrong if that is a different video. Re-compose the CURRENT key and attach only if it still
        // matches the pinned one — same context ⇒ same key. If it drifted, the .srt is already cached and
        // will load on that video's next play, so nothing is lost by staying quiet.
        const QString nowIdent = SubtitleFetcher::cacheIdentifier(self->subCtx_.imdbStreamId, self->subCtx_.title,
                                                                  self->subCtx_.localPath);
        if (SubtitleCache::keyFor(nowIdent, lang) != cacheKey)
        {
            if (self->notifier_) self->notifier_->hideNotice();
            return;
        }
        self->player_->addSubtitle(srt);
        self->notify(tr("Subtitle added."), 3000);
    });
}
