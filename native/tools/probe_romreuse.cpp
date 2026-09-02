// Headless check of WHICH FILE IN A CONSOLE'S ROMs FOLDER IS AN OWNED COPY OF A GAME (issue #236).
//
// WHY THIS PROBE EXISTS. A bridged NES leaf ("Tetris", browsed from a metadata catalog whose ROMs come from
// a file provider) resolved to a release called "Tetris.zip". The remote-open path then asked whether the
// machine already owned the game by stat'ing exactly one path — "<roms>/nes/<title><the release's
// extension>" — so it looked for "Tetris.zip", missed the "Tetris.nes" and "Tetris.7z" sitting beside it,
// went to the network for a ROM already on the disk, and got nothing back. openGamePath was never called:
// not one "game:" line was written for the press, on either layout, which is why it read as a dead button.
// A sibling leaf whose release happened to resolve to ".nes" played instantly off the same folder.
//
// The routing question is therefore "is a copy of THIS GAME here", not "is a copy under THAT extension
// here", and romreuse::pickLocalCopy is the one place it is answered. It is pure — a base name, the
// resolver's preferred extension, the accepted extensions and a directory listing — so every case below is
// statable with no ROMs folder, no addon and no window.
//
// WHAT IT PINS:
//   §1 THE #236 CASE. A ".zip" release finds the ".nes" already on disk.
//   §2 NO REGRESSION. When the preferred extension IS present it still wins outright, so every play that
//      resolved locally before resolves to the byte-identical file.
//   §3 PREFERENCE ORDER. Accepted-extension order decides, not directory order: a plain ROM beats the same
//      game packed in an archive even when the archive is listed first.
//   §4 THE ARCHIVE FALLBACK. Packed-only is still owned — GameLauncher unpacks it on launch.
//   §5 THE NEGATIVES, which are the whole safety of widening the lookup: a base name is matched WHOLE
//      ("Tetris" never picks up "Tetris 2" or "Tetrisphere"), an extension outside the accepted list is
//      never a ROM (cover art, a save, a gamelist), and an empty title claims nothing.
//   §6 SPELLING. A leading dot on the preferred extension is optional and case never decides.
#include "RomReuse.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <cstdio>

static int g_fails = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) { std::printf("ROMREUSE-FAIL %s (%d)\n", #cond, __LINE__); ++g_fails; } \
    } while (0)

namespace {

// The NES entry of SystemCatalog's built-in table, then the archives GameLauncher unpacks — the exact list
// MainWindow::romsFolderExistingFor builds, in the same order (ROMs first, archives last).
const QStringList kNesAccepted = { QStringLiteral("nes"), QStringLiteral("fds"), QStringLiteral("unif"),
                                   QStringLiteral("unf"), QStringLiteral("zip"), QStringLiteral("7z") };

// The real folder the fault was found in, listed as QDir would (by name).
const QStringList kRealNesFolder = {
    QStringLiteral("Arkanoid (Arkanoid (J) - Revised by nesrocks v103).nes"),
    QStringLiteral("Arkanoid (J) [T-Port].nes"),
    QStringLiteral("Super Mario Bros. 3 (Super Mario Bros 3 A New Journey).nes"),
    QStringLiteral("Super Mario Bros. 3.7z"),
    QStringLiteral("Tetris (Tetris).nes"),
    QStringLiteral("Tetris.7z"),
    QStringLiteral("Tetris.nes"),
    QStringLiteral("The Great Gatsby.nes"),
    QStringLiteral("gamelist.xml"),
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- §1 the #236 case: a ".zip" release, a ".nes" on disk --------------------------------------------
    {
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris"), QStringLiteral(".zip"),
                                      kNesAccepted, kRealNesFolder) == QStringLiteral("Tetris.nes"));
        // The neighbour that always worked, from the same listing — the asymmetry that made this a bug.
        CHECK(romreuse::pickLocalCopy(QStringLiteral("The Great Gatsby"), QStringLiteral(".nes"),
                                      kNesAccepted, kRealNesFolder) == QStringLiteral("The Great Gatsby.nes"));
        // A title whose only copy is packed, from the same listing.
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Super Mario Bros. 3"), QStringLiteral(".zip"),
                                      kNesAccepted, kRealNesFolder) == QStringLiteral("Super Mario Bros. 3.7z"));
    }

    // ---- §2 no regression: the preferred extension still wins outright -----------------------------------
    {
        const QStringList entries = { QStringLiteral("Tetris.nes"), QStringLiteral("Tetris.fds") };
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris"), QStringLiteral(".fds"),
                                      kNesAccepted, entries) == QStringLiteral("Tetris.fds"));
        // ...even when the preferred extension is not in the accepted list at all: it is a file the caller
        // asked for by name, and answering it keeps the pre-#236 stat exactly.
        const QStringList odd = { QStringLiteral("Tetris.rom") };
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris"), QStringLiteral(".rom"),
                                      kNesAccepted, odd) == QStringLiteral("Tetris.rom"));
    }

    // ---- §3 preference order is the ACCEPTED list's, not the listing's -----------------------------------
    {
        const QStringList archiveFirst = { QStringLiteral("Tetris.7z"), QStringLiteral("Tetris.zip"),
                                           QStringLiteral("Tetris.nes") };
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris"), QString(),
                                      kNesAccepted, archiveFirst) == QStringLiteral("Tetris.nes"));
        // An empty preferred extension is not a wildcard for the first thing in the folder.
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris"), QString(),
                                      kNesAccepted, kRealNesFolder) == QStringLiteral("Tetris.nes"));
    }

    // ---- §4 packed-only is still owned -------------------------------------------------------------------
    {
        const QStringList packed = { QStringLiteral("Tetris.7z") };
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris"), QStringLiteral(".zip"),
                                      kNesAccepted, packed) == QStringLiteral("Tetris.7z"));
    }

    // ---- §5 the negatives: what widening the lookup must NOT start claiming ------------------------------
    {
        // Whole-name match. Both of these are DIFFERENT GAMES that share a prefix, and both sit in real
        // folders beside the one being asked for.
        const QStringList neighbours = { QStringLiteral("Tetris 2.nes"), QStringLiteral("Tetrisphere.nes"),
                                         QStringLiteral("Tetris Attack.zip") };
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris"), QStringLiteral(".zip"),
                                      kNesAccepted, neighbours).isEmpty());
        // ...and the reverse direction: the shorter name must not answer for the longer one either.
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris 2"), QStringLiteral(".zip"), kNesAccepted,
                                      { QStringLiteral("Tetris.nes") }).isEmpty());

        // An extension outside the accepted list is never a ROM: the art, the save and the scrape index all
        // live in the same folder under the game's own name.
        const QStringList notRoms = { QStringLiteral("Tetris.png"), QStringLiteral("Tetris.srm"),
                                      QStringLiteral("Tetris.txt"), QStringLiteral("Tetris.state") };
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris"), QStringLiteral(".zip"),
                                      kNesAccepted, notRoms).isEmpty());

        // An empty listing, an empty accepted list, and an empty title all claim nothing.
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris"), QStringLiteral(".zip"),
                                      kNesAccepted, {}).isEmpty());
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris"), QString(), {}, kRealNesFolder).isEmpty());
        CHECK(romreuse::pickLocalCopy(QString(), QStringLiteral(".nes"),
                                      kNesAccepted, kRealNesFolder).isEmpty());

        // A file with no extension at all is not claimed by an accepted-extension pass.
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris"), QString(), kNesAccepted,
                                      { QStringLiteral("Tetris") }).isEmpty());
    }

    // ---- §6 spelling: the dot is optional, the case is not load-bearing ----------------------------------
    {
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris"), QStringLiteral("nes"), kNesAccepted,
                                      { QStringLiteral("Tetris.nes") }) == QStringLiteral("Tetris.nes"));
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Tetris"), QStringLiteral(".ZIP"), kNesAccepted,
                                      { QStringLiteral("TETRIS.NES") }) == QStringLiteral("TETRIS.NES"));
        CHECK(romreuse::pickLocalCopy(QStringLiteral("tetris"), QStringLiteral(".zip"), kNesAccepted,
                                      { QStringLiteral("Tetris.Nes") }) == QStringLiteral("Tetris.Nes"));
        CHECK(romreuse::normaliseExt(QStringLiteral(".NES")) == QStringLiteral("nes"));
        CHECK(romreuse::normaliseExt(QStringLiteral("NES")) == QStringLiteral("nes"));
        CHECK(romreuse::normaliseExt(QString()).isEmpty());
    }

    // ---- a title with dots in it: "Super Mario Bros. 3" is a base name, not a name plus an extension ------
    {
        CHECK(romreuse::pickLocalCopy(QStringLiteral("Super Mario Bros. 3"), QString(), kNesAccepted,
                                      { QStringLiteral("Super Mario Bros. 3.nes") })
              == QStringLiteral("Super Mario Bros. 3.nes"));
    }

    if (g_fails) { std::printf("ROMREUSE: %d failure(s)\n", g_fails); return 1; }
    std::printf("ROMREUSE-OK\n");
    return 0;
}
