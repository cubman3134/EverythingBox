// Installing a romhack as a playable game. A hack is a patch, not a ROM: to play it we apply the patch to a
// base ROM and write the result into the ROMs folder as a real file. From that moment the hack IS an ordinary
// local game — the library scan finds it, and tiles, saves, save states, marks, tags, stats, favourites and
// scraping all work with no new identity plumbing. The alternative, a virtual entry patched at launch, would
// have meant threading a new id through every per-item store for the sake of saving a few megabytes.
//
// The base ROM is never modified: the patched game is a NEW file beside it, and every refusal RomPatch makes
// (bad magic, malformed patch, a BPS/UPS checksum built for a different dump) happens before anything is
// written, so a refused install cannot leave a half-made ROM in the library looking playable.
//
// Deliberately NOT this unit's job: writing the hack's metadata. GamelistStore::write() takes a MediaDetail
// and downloads remote art, which is the caller's material and the caller's network — the UI layer holds the
// base game's MediaDetail already, so it writes the gamelist entry after a successful install.
#pragma once
#include <QByteArray>
#include <QString>

namespace RomhackInstall
{
    // A hack's title reduced to something safe to put in a file name: path separators, wildcards and the
    // other characters Windows reserves become spaces, runs of space collapse, and the result is trimmed and
    // length-capped. Returns an empty string when nothing usable survives, which install() treats as an error
    // rather than silently writing "Game ().sfc".
    QString sanitizeHackTitle(const QString& title);

    // Where a hack installs to: `<targetDir>/<base name> (<sanitised hack title>).<base extension>`. The base
    // ROM's own extension is kept because patching does not change the container, and the emulator resolves
    // the system from it downstream. `targetDir` is normally the base ROM's own folder (already the right
    // per-system folder for anything in the ROMs library); pass a different one to install elsewhere.
    // Returns an empty string if the hack title sanitises away to nothing.
    QString destinationFor(const QString& baseRomPath, const QString& hackTitle, const QString& targetDir);

    // Verify, apply, and write the patched game. Returns its path, or an empty string with *error set.
    //
    // Idempotent: installing the same hack onto the same ROM twice yields the same path with the same bytes
    // and no second file, so a re-install after an interrupted download does not litter the library with
    // "Game (Hack) (1).sfc".
    QString install(const QString& baseRomPath, const QByteArray& patch, const QString& hackTitle,
                    const QString& targetDir, QString* error = nullptr);
}
