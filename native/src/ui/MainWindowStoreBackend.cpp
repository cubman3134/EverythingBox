// Store backends (issue #118, increment 1), the MainWindow half — a SEPARATE translation unit that defines
// MainWindow's #118 members, for the reason MainWindowPlayOn.cpp gives at length: MainWindow.cpp is the
// busiest merge surface in the repository and everything here reaches the class only through members that
// already existed, so it costs that file three short insertions per settings builder instead of a hundred
// lines.
//
// WHAT THE SETTINGS SURFACE OWES THE USER, and it is the whole point of this file: an HONEST answer when
// nothing is set up. "Your Epic library is empty" is a lie on a machine with no legendary installed, and it
// is the lie a feature like this tells by default, because the absent case is the one nobody drives. So the
// row says which of the three states it is in — not installed / installed but signed out / signed in — and
// the not-installed state links legendary's own releases page rather than silently downloading a binary.
//
// GUI-THREAD DISCIPLINE. legendaryStatusLine() is a file lookup and an ini read: no process, so a panel open
// costs nothing. Everything that spawns legendary (the sign-in) goes through storeback::signInAsync, off the
// GUI thread, with the result delivered back here.
//
// THE CREDENTIAL. The authorization code the user pastes is a credential. It is read into a local, handed to
// storeback::signInAsync, and dropped. It is not stored, not logged, and never interpolated into a status
// line or a notice — the notice says whether it worked, and nothing else. What legendary does with it (a
// refresh token in legendary's own config directory) is legendary's business and is deliberately not ours.
#include "MainWindow.h"

#include <QLineEdit>
#include <QPointer>
#include <QTimer>

#include "../core/LegendaryBackend.h"
#include "../core/StoreBackend.h"
#include "nav/Osk.h"

QString MainWindow::legendaryStatusLine() const
{
    StoreBackend* b = storeback::legendary();
    // FRONT-LOADED, all three of them. A themed Info row elides at about ninety characters, so the state a
    // person is checking for has to be the first thing in the sentence and not the payoff at the end.
    if (b->toolPath().isEmpty())
        return tr("Not installed. Without it, an %1 library needs that store's own launcher — which Linux "
                  "does not have.").arg(b->storeName());
    // The cached flag, not a fresh ask: this runs while the panel is being built. It is written by the
    // owned-library refresh and by a successful sign-in, so it is at most one refresh out of date.
    if (!storeback::cachedSignedIn(b->id()))
        return tr("Installed, but not signed in to %1 yet.").arg(b->storeName());
    return tr("Installed and signed in. Owned %1 games appear in PC Games.").arg(b->storeName());
}

void MainWindow::openLegendaryReleases()
{
    StoreBackend* b = storeback::legendary();
    // The URL is always SHOWN, never only handed to a browser: on a TV form factor openAuthPage does nothing
    // on purpose, and a row that silently did nothing there would read as broken. (Same rule as #192's
    // device-code row.)
    notify(tr("Get %1 from %2, then put it on your PATH or in the app's tools folder.")
           .arg(b->toolName(), b->releasesUrl()), 12000);
    openAuthPage(b->releasesUrl());
}

void MainWindow::promptLegendaryAuth(const std::function<void(const QString&)>& setStatus)
{
    StoreBackend* b = storeback::legendary();
    if (b->toolPath().isEmpty())
    {
        notify(storeback::reasonFor(StoreStatus::ToolMissing, b->toolName(), b->storeName()), 8000);
        return;
    }

    // DEFER PAST THIS EMISSION. We are inside a settings-row activation (a themed panel action delivery or a
    // QPushButton::clicked), and Osk::getText spins a NESTED EVENT LOOP — running it under the frame that is
    // still delivering the signal is the #28 / #211 crash family. One singleShot puts it on its own stack.
    QPointer<MainWindow> self(this);
    QTimer::singleShot(0, this, [self, b, setStatus] {
        if (!self) return;
        self->notify(self->tr("Sign in to %1 at %2, then paste the authorizationCode from the page it lands on.")
                     .arg(b->storeName(), b->signInUrl()), 20000);
        self->openAuthPage(b->signInUrl());

        // Password echo: this is a credential and a living-room screen is a public one.
        const QString code = Osk::getText(self->tr("Paste the authorizationCode:"), QString(),
                                          QLineEdit::Password, self);
        if (code.trimmed().isEmpty()) return;   // backed out or pasted nothing: nothing runs, nothing is kept

        self->notify(self->tr("Signing in to %1…").arg(b->storeName()), 8000);
        storeback::signInAsync(b, code, self, [self, b, setStatus](StoreStatus st) {
            if (!self) return;
            if (st == StoreStatus::Ok)
                self->notify(self->tr("Signed in to %1.").arg(b->storeName()), 6000);
            else if (st == StoreStatus::Failed)
                // Deliberately not the child's stderr: legendary echoes the request it made, and that
                // request carries the code.
                self->notify(self->tr("%1 didn't accept that code. They expire quickly — try the sign-in "
                                      "again for a fresh one.").arg(b->toolName()), 9000);
            else
                self->notify(storeback::reasonFor(st, b->toolName(), b->storeName()), 8000);
            if (setStatus) setStatus(self->legendaryStatusLine());
        });
    });
}
