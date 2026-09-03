// Platform-aware ROM routing (issue #53). The folder under <root>/<system>/ IS the platform declaration:
// a file found there belongs to that system, full stop. So on a FOLDER scan the extension list stops being
// the router and becomes only a filter for the NON-ROM sidecars that live alongside ROMs — saves, patches,
// box art, metadata, temp files — which must never become library tiles. This lets PS2/PSP/Dreamcast/Xbox
// folders pick up their own .iso/.chd/.cso/.gdi/.cue without having to declare extensions that collide with
// earlier systems (the collision that made those systems unable to claim their own most common format).
//
// This is the PURE, testable heart (QtCore-only, no disk I/O, no clock, no ini), mirroring DiscGroup.h /
// HashVerify.h. The scan glue in RomLibrary::scan() does the I/O and calls acceptUnderSystemFolder() to
// decide each file. Loose files handed to the app WITHOUT folder context (drag-and-drop, "Open game file…")
// still route by extension + console hint through SystemCatalog::forExtension / forConsoleName — this header
// deliberately does NOT touch that path, so a bare .iso with no folder and no hint routes exactly as before.
#pragma once
#include <QChar>
#include <QSet>
#include <QString>
#include <QStringList>

namespace RomRouting
{
    // True for a lowercase extension (no leading dot) that is a NON-ROM sidecar commonly found in a ROM
    // folder and must NEVER become a library entry. CONSERVATIVE by design: when an extension could be a
    // real ROM/disc format it is NOT junked — a false-junk hides a real game, while a false-accept only
    // surfaces one stray tile. Notable DELIBERATE non-members (kept as ROMs, never junked):
    //   * "bin"  — a real raw disc/ROM image (Mega-CD/PS1 track, headered dumps). The brief flags it ambiguous.
    //   * "md"   — Sega Mega Drive ROM extension (collides with Markdown). Junking it would hide Genesis games,
    //              so it is NOT in the metadata set even though .md is also a docs format.
    //   * "st","sv","do","car","cas","min","col","gg", … — real ROM extensions of other systems; absent below.
    // Any extension the built-in SystemCatalog claims as a ROM stays out of this set on purpose.
    inline bool isLibraryJunkExtension(const QString& extLower)
    {
        if (extLower.isEmpty()) return false; // no extension -> the folder says it's a game; not junk

        // Numbered save states written next to a ROM: RetroArch "<rom>.state1".."<rom>.state9"/"stateN" and
        // the "<rom>.ss0".."ss9" slot family. Matched by shape so every N is covered without listing each.
        if (extLower.size() > 5 && extLower.startsWith(QLatin1String("state")))
        {
            bool allDigits = true;
            for (int i = 5; i < extLower.size(); ++i)
                if (!extLower.at(i).isDigit()) { allDigits = false; break; }
            if (allDigits) return true; // "state1".."state42"
        }
        if (extLower.size() == 3 && extLower.startsWith(QLatin1String("ss")) && extLower.at(2).isDigit())
            return true; // ss0..ss9

        static const QSet<QString> junk = {
            // --- saves & memory cards ---------------------------------------------------------------------
            QStringLiteral("srm"), QStringLiteral("sav"), QStringLiteral("state"),
            QStringLiteral("mcr"), QStringLiteral("mcd"), QStringLiteral("vmp"),
            // --- patches (applied TO a ROM, not playable on their own) ------------------------------------
            QStringLiteral("ips"), QStringLiteral("bps"),
            // --- art / media sidecars ---------------------------------------------------------------------
            QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("gif"),
            QStringLiteral("bmp"),  QStringLiteral("webp"), QStringLiteral("mp4"),  QStringLiteral("webm"),
            QStringLiteral("pdf"),
            // --- metadata (note: "md" is intentionally NOT here — it is a Mega Drive ROM extension) --------
            QStringLiteral("xml"),  QStringLiteral("txt"),  QStringLiteral("nfo"),  QStringLiteral("dat"),
            QStringLiteral("json"), QStringLiteral("ini"),  QStringLiteral("cfg"),  QStringLiteral("db"),
            QStringLiteral("log"),
            // --- temp / backup ----------------------------------------------------------------------------
            // ".part" is a transfer STILL IN PROGRESS, not a file: DownloadManager streams into "<dest>.part"
            // and only renames on success, keeping it across a failure or a pause so a retry can resume. The
            // ROMs folder is now a download DESTINATION as well as a scanned library (a hack published as a
            // finished game lands there through the ordinary queue), so a scan runs while one exists — and
            // accepting it would put a truncated ROM in the library under the game's own name, playable-looking
            // and handed straight to an emulator. A paused or failed job would leave that tile there forever.
            QStringLiteral("tmp"),  QStringLiteral("bak"),  QStringLiteral("part"),
        };
        return junk.contains(extLower);
    }

    // The folder-authoritative accept decision for RomLibrary::scan(): a file found under a recognised
    // <root>/<system>/ folder belongs to that system iff it is not junk. This is the ONE place the scan
    // consults, so the "folder is the platform declaration" rule lives here as a named, testable function
    // rather than an inline allowlist. zip/7z are not junk, so archived ROMs are accepted as before.
    inline bool acceptUnderSystemFolder(const QString& extLower)
    {
        return !isLibraryJunkExtension(extLower);
    }

    // ---- the CONTENT SIDECAR folders (issue #189) ---------------------------------------------------------
    // `updates/` and `dlc/` beside a game hold that game's update and DLC PACKAGES, not games. They are
    // ordinary ROM-shaped files (.nsp, .wux, a folder of .rpx) sitting under a recognised system folder, so
    // the extension filter above cannot tell them apart from the game — the FOLDER is what says what they are,
    // exactly as the folder is what says which system a file belongs to.
    //
    // Without this the feature is self-defeating: putting a game's update beside it would add a second tile to
    // the library named after the update, and "Great Game [0100000000010800][v65536]" would sit in the grid
    // looking playable next to the game it patches. (Observed on the very first live drive of #189.)
    //
    // `relPath` is the file's path RELATIVE to the system folder, with '/' separators. Case-insensitive, and
    // it matches a whole segment only — a game legitimately called "Updates.nsp", or one in a folder called
    // "dlcpack", is untouched.
    inline bool underContentSidecar(const QString& relPath)
    {
        const QStringList parts = relPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (int i = 0; i + 1 < parts.size(); ++i)   // directory segments only — never the file name itself
            if (parts.at(i).compare(QLatin1String("updates"), Qt::CaseInsensitive) == 0
             || parts.at(i).compare(QLatin1String("dlc"), Qt::CaseInsensitive) == 0)
                return true;
        return false;
    }
}
