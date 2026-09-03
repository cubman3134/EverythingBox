// THE SELF-COMPILED TIER'S CARD (issue #248, increment b).
//
// A catalogue entry that names a recompiler (`build.generate.engine` — psxrecomp / snesrecomp / gbarecomp) is
// not a download. The port does not exist anywhere as a binary somebody could hand you; it is PRODUCED on this
// machine, by that engine, out of the dump you already own. Increment (c) is where that happens.
//
// So this increment lists those entries and stops honestly. The alternative — reusing the pre-built card —
// would offer "Install and play" and then talk about a release download, an unsigned binary and Windows
// Defender, none of which is about the operation the button would start. A verb that cannot run is allowed to
// exist and say so; a verb that lies about what it is going to do is not.
//
// WHAT THE CARD SAYS, and each line is here because a person deciding needs it BEFORE anything happens:
//   * which engine builds it, by name. A self-compiled port is that project's program as much as the port
//     author's, and the credit belongs on screen either way;
//   * under what LICENCE that engine is. All three named engines are PolyForm Noncommercial 1.0.0, which is a
//     term with actual consequences for a user, and #248 named psxrecomp's case specifically. Nothing of any
//     engine is bundled in this app and this increment fetches none of it — the engine is named and linked,
//     and that is the whole of this build's relationship with it;
//   * that building on this machine arrives in a later update, in those words, with the engine's own page
//     offered for somebody who wants to do it by hand today.
//
// In its OWN translation unit rather than appended to MainWindow.cpp: that file is one giant TU that a dozen
// branches edit at once, and a feature that can live beside it should (#186's direction). Only the declaration
// is in MainWindow.h.
#include "MainWindow.h"

#include <QDesktopServices>
#include <QUrl>

#include "../core/RecompFeed.h"
#include "nav/NavOverlay.h"

void MainWindow::showSelfCompiledPort(const ExternalEmulator& port)
{
    const RecompFeed::Engine engine = RecompFeed::engineInfo(port.port.buildEngine);
    // The port project's own name where the catalogue gives one, never the recompiler's brand as the title
    // (#233: those developers asked a third-party launcher for exactly that).
    const QString heading = port.displayName.isEmpty() ? port.port.name : port.displayName;

    QStringList lines;
    lines << tr("%1 is a recompilation of “%2” that is BUILT ON THIS COMPUTER — there is no download of the "
                "finished program. It is made from your own copy of the game by a separate recompiler, and it "
                "is not made by EverythingBox.")
                 .arg(heading, port.port.name);
    if (!port.port.description.isEmpty()) lines << port.port.description;

    // The engine, and its terms. Only what has been checked: an engine this build does not know keeps an
    // empty licence, and the line is left out rather than guessed at.
    if (!engine.id.isEmpty())
    {
        lines << (engine.license.isEmpty()
                      ? tr("It is built with %1.").arg(engine.id)
                      : tr("It is built with %1, which is licensed %2. EverythingBox does not include or "
                           "download %1 — it would be run from the project's own release.")
                            .arg(engine.id, engine.license));
    }
    if (!port.port.authorNotes.isEmpty()) lines << port.port.authorNotes;

    lines << tr("Building on this machine arrives in a later update. Until then this row is here so you can "
                "see the title exists and which dump it needs.");

    QStringList buttons{ tr("OK") };
    // The engine's page first when there is one — that is where somebody who wants it today has to go — and
    // the port project's own page after it.
    const bool haveEngine = !engine.homepage.isEmpty();
    if (haveEngine)                 buttons << tr("Open %1").arg(engine.id);
    if (!port.homepage.isEmpty())   buttons << tr("Open homepage");

    const int choice = NavConfirm::ask(heading, lines.join(QStringLiteral("\n\n")), buttons,
                                       /*focusIndex*/ 0, /*cancelIndex*/ 0, this);
    if (choice <= 0) return;
    if (haveEngine && choice == 1) { QDesktopServices::openUrl(QUrl(engine.homepage)); return; }
    if (!port.homepage.isEmpty())  QDesktopServices::openUrl(QUrl(port.homepage));
}
