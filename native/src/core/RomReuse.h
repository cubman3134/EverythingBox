// #236: WHICH FILE IN A CONSOLE'S ROMs FOLDER IS AN ALREADY-OWNED COPY OF A GAME.
//
// The remote-open path (MainWindow::fetchRemoteDocumentThenOpen) skips the network entirely when the ROMs
// folder already holds the game. It used to ask that question as a single stat of
// "<title><the extension of the release the resolver just picked>" — and a release's extension is not a
// property of the game at all. A bridged NES leaf resolved to "Tetris.zip", so the app stat'd
// "<roms>/nes/Tetris.zip", missed the "Tetris.nes" (and "Tetris.7z") sitting right beside it, went to the
// network for a copy it already had, and the source returned nothing. The whole press then ended on a toast:
// openGamePath was never called, so not one "game:" line was written and it read as a dead button. The same
// leaf whose release happened to resolve to ".nes" played instantly off disk. That asymmetry — same shelf,
// same console, same backend, decided by the extension a remote source picked — is the bug.
//
// So the question is asked over the DIRECTORY LISTING instead: an entry is an owned copy when its complete
// base name is the game's, and its extension is one this console's ROMs are filed under (or an archive one
// of them is packed in — GameLauncher extracts those on launch). The preferred extension still wins outright
// when it is there, so an item that resolved locally before still resolves to the same byte-identical file.
//
// PURE AND QtCore-ONLY on purpose: no Settings, no filesystem, no SystemCatalog. The caller supplies the
// listing and the accepted extensions, which is what lets probe_romreuse state every case below without a
// ROMs folder, a window or an addon — including the two that must NOT match, which are the ones a careless
// widening would break (a base name is matched WHOLE, so "Tetris" never picks up "Tetris 2").
#pragma once
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace romreuse
{

// Strip a leading dot and lower-case, so ".NES", "NES" and "nes" are one extension.
inline QString normaliseExt(const QString& ext)
{
    QString e = ext.trimmed().toLower();
    if (e.startsWith(QLatin1Char('.'))) e = e.mid(1);
    return e;
}

// The entry in `entries` (bare file names from one console's ROMs folder) that is an owned copy of
// `baseName`, or "" when there is none.
//
//   1. "<baseName><preferredExt>" if it is present — the pre-#236 answer, kept exactly;
//   2. else the first entry whose extension appears in `acceptedExts`, tried in ACCEPTED-EXTENSION order so
//      the caller decides the preference (a plain ROM ahead of an archive), not the directory order.
//
// `acceptedExts` are lowercase and dot-less; `preferredExt` may carry a leading dot and may be empty.
// Comparison is case-insensitive: a ROMs folder is user-managed, and "TETRIS.NES" is the same copy.
inline QString pickLocalCopy(const QString& baseName, const QString& preferredExt,
                             const QStringList& acceptedExts, const QStringList& entries)
{
    if (baseName.isEmpty()) return QString();

    auto isTitled = [&baseName](const QString& entry) {
        return QFileInfo(entry).completeBaseName().compare(baseName, Qt::CaseInsensitive) == 0;
    };

    const QString preferred = normaliseExt(preferredExt);
    if (!preferred.isEmpty())
        for (const QString& entry : entries)
            if (isTitled(entry) && QFileInfo(entry).suffix().compare(preferred, Qt::CaseInsensitive) == 0)
                return entry;

    for (const QString& want : acceptedExts)
    {
        const QString w = normaliseExt(want);
        if (w.isEmpty()) continue;
        for (const QString& entry : entries)
            if (isTitled(entry) && QFileInfo(entry).suffix().compare(w, Qt::CaseInsensitive) == 0)
                return entry;
    }
    return QString();
}

} // namespace romreuse
