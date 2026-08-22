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
    // `baseNameOverride`, when non-empty, replaces the name part. That is for the archived case: the patch is
    // applied to a ROM EXTRACTED to a temp file, whose name is whatever was inside the archive, but the
    // installed game should be named after the library entry the user recognises. The extension still comes
    // from baseRomPath — the extracted ROM's — because that is what the emulator resolves the system from,
    // and it is precisely the thing the archive's own ".7z" would have got wrong.
    QString destinationFor(const QString& baseRomPath, const QString& hackTitle, const QString& targetDir,
                           const QString& baseNameOverride = QString());

    // Where a FINISHED ROM installs to: `<targetDir>/<sanitised title>.<ext>`. No base name, because there
    // is no base ROM in this story — the title already names the game ("Arkanoid (J) [T-Port]"), and pairing
    // it with the game it derives from would read "Arkanoid (Arkanoid (J) [T-Port])". `ext` comes from the
    // file the source named, since that is the only statement of what container this is.
    // Returns an empty string if the title sanitises away to nothing.
    QString destinationForRom(const QString& title, const QString& ext, const QString& targetDir);

    // Write a hack that was published as a FINISHED GAME rather than as a patch. Some collections distribute
    // the playable result, and then there is nothing to apply: no base ROM to find, no dump to match, and no
    // checksum question to put to anyone. The bytes are the game.
    //
    // Deliberately a separate entry point rather than a flag on install(): every refusal install() makes is
    // about a patch meeting a ROM, and none of them means anything here. One function that skipped half its
    // own safety checks depending on an argument would be the more dangerous shape.
    //
    // Idempotent in the same way install() is — the path is a function of (title, ext), so re-installing
    // rewrites the same file rather than adding "Game (1).nes".
    QString installRom(const QByteArray& rom, const QString& title, const QString& ext,
                       const QString& targetDir, QString* error = nullptr);

    // Does this ROM match the dump a source SAID a patch was built for? Hex strings, either may be empty;
    // SHA-1 wins when both are given, being the stronger of the two. False when the file cannot be read, when
    // neither hash was stated (there is nothing to match), or when it simply does not match.
    //
    // This is the difference between installing a hack and hoping. IPS carries no checksum and applies
    // cleanly to any bytes at all, so a patch built for another dump of the same game yields a broken game
    // with nothing to catch it — most often a translation, which is built against the release that needed
    // translating rather than the one most libraries hold.
    bool romMatches(const QString& romPath, const QString& crc32Hex, const QString& sha1Hex);

    // Verify, apply, and write the patched game. Returns its path, or an empty string with *error set.
    //
    // Idempotent: installing the same hack onto the same ROM twice yields the same path with the same bytes
    // and no second file, so a re-install after an interrupted download does not litter the library with
    // "Game (Hack) (1).sfc".
    QString install(const QString& baseRomPath, const QByteArray& patch, const QString& hackTitle,
                    const QString& targetDir, QString* error = nullptr,
                    const QString& baseNameOverride = QString());
}
