// Transparently turn a .zip / .7z ROM into a plain ROM file the emulators can load. MyMediaVault's
// libretro cores and external emulators all take a file path (or read the file into memory), so the
// universal way to support archived ROMs is to extract the inner ROM to a temp file once and hand that
// path on. .zip uses the bundled miniz; .7z uses the vendored LZMA SDK (see SevenZip) — both portable C,
// so this works on Windows/macOS/Linux/Android/iOS with no system tools.
#pragma once
#include <QString>
#include <QStringList>

namespace ArchiveRom
{
    // True if the path looks like a supported archive (.zip or .7z) by extension.
    bool isArchive(const QString& path);

    // Extract the ROM from the archive to a temp file and return its path (its name preserves the inner
    // file's, so the extension is intact for system detection). Picks the member matching wantedExts
    // (each like ".sfc"); with none given, the largest non-junk file (archives usually hold one ROM).
    // Re-extraction is skipped if the temp file already exists from a previous open. Empty on failure;
    // *error (if given) holds a message.
    QString extractToTemp(const QString& archivePath, const QStringList& wantedExts = {}, QString* error = nullptr);

    // Extract every file in a .zip/.7z archive under destDir, preserving internal paths — for a whole
    // release like a PC-game repack (setup.exe + its parts). Returns false on the first failure.
    bool extractAll(const QString& archivePath, const QString& destDir, QString* error = nullptr);
}
