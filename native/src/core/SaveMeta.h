// Per-save metadata: the <data>/saves-meta.json sidecar that says WHICH GAME a save file belongs to, plus
// the two rules that decide where a save file lives at all.
//
// WHY THE SIDECAR EXISTS. Saves are named after the ROM's completeBaseName, and a ROM downloaded through the
// app is cached under a 40-hex hash — so most real .srm files on disk are named for a file that may no longer
// exist. "101306d4….srm" is not something a user can act on. The sidecar records {title, system, romPath,
// updatedAt} per save as it is written, which turns that name back into "Zelda II" for the conflict notice
// SaveSync raises (Task 2 already calls titleFor there), and keeps working after the ROM is deleted.
//
// EXISTING FILES ARE NEVER RENAMED. The sidecar is purely additive: a save with no entry displays its own
// file name. Renaming saves under a user is the kind of irreversible tidying this whole track refuses.
//
// The sidecar is DEVICE-LOCAL — it is not in the sync bundle and not in saves-index.json. A device that never
// ran the game therefore has no title for it and falls back to the file name, which is exactly what titleFor
// promises. It is not a cache of anything remote, so there is nothing to reconcile.
//
// resolvePath() and sweepStrays() live here rather than in RetroView because RetroView is a QWidget that
// cannot be linked into a headless probe, and both are file-placement rules whose failure mode is a save the
// user can no longer see: probe_savesync covers them here.
//
// Thread-safety: put/lookup/titleFor serialise on one internal mutex. saveSram() runs on the emulation WORKER
// thread in split-screen mode, so put() genuinely is called from more than one thread. That mutex is
// PROCESS-LOCAL: the app is portable, so two instances doing read-modify-write on saves-meta.json are
// last-writer-wins over the WHOLE document — each write is atomic (QSaveFile), but an entry the other instance
// added between this one's read and write is lost. Acceptable because the sidecar is descriptive only: the
// worst outcome is a save displaying its file name until the next put() re-records it.
#pragma once
#include <QString>

namespace SaveMeta
{
    // What the sidecar records about one save file.
    struct Entry
    {
        QString title;         // display name of the game ("Zelda II"), never derived from the save's own name
        QString system;        // system id ("nes", "snes", …); empty when the launcher could not resolve one
        QString romPath;       // the ROM as it was when the save was written (it may be gone by now)
        qint64  updatedAt = 0; // epoch ms of the last write
        bool recorded() const { return updatedAt != 0 || !title.isEmpty(); }
    };

    // `relPath` throughout is the save's path RELATIVE TO AppPaths::dataDir(), INCLUDING the "saves/" or
    // "states/" prefix — "saves/nes/Zelda II.srm", "states/Zelda II.state1". That is deliberate and not the
    // more obvious "relative to saves/": it is the exact key SaveSync::scanLocal produces and the cloud index
    // uses, and SaveSync already calls titleFor() with it. Keying on anything else would look right and match
    // nothing.
    void  put(const QString& relPath, const QString& title, const QString& system, const QString& romPath);
    Entry lookup(const QString& relPath);   // a default-constructed Entry when nothing is recorded

    // The sidecar key for an ABSOLUTE save path: that same data-dir-relative spelling, derived in ONE place.
    // Callers that have an absolute path (RetroView after writing a save, resolvePath inspecting a candidate)
    // must not each re-derive it — a second, differently-derived key would record and look up different rows
    // for one file and every lookup would silently miss.
    QString keyFor(const QString& absPath);

    // The display title for a save file, or the bare file name when nothing is recorded. Never empty.
    QString titleFor(const QString& relPath);

    // Where a save lives, tolerating BOTH layouts. New saves go under <root>/<systemId>/ so two systems
    // sharing a ROM base name stop colliding — but a save written before that change lives flat in <root>,
    // and silently failing to find it would look to the user exactly like their save being wiped. So: prefer
    // the namespaced path, fall back to the legacy flat path when it exists, and only ever CREATE namespaced.
    // A legacy save is left where it is and keeps being written in place; nothing is migrated.
    // `ext` includes the dot (".srm"). An empty `systemId` (unknown system) yields the flat path — never a
    // path with an empty component, which would produce a "saves//x.srm" sync key that matches nothing.
    //
    // And before CREATING a namespaced save, every OTHER system's namespace is searched for the same base
    // name. `systemId` describes how the user navigated to the ROM (systemHint, else the first catalog entry
    // claiming the extension), not the ROM itself, so the same file can arrive here under two different
    // systems — and without that search the second launch would start a fresh empty save while the real one
    // sat unreachable under the first. One match is used as-is; several (different games sharing a base name)
    // resolve to the most recently modified, and that ambiguity is logged.
    //
    // `romPath` is what GATES that adoption, and it is load-bearing: frontend SRAM is always ".srm", so the
    // base name alone cannot tell "the same ROM opened as a second system" from two different games that
    // merely share a name ("Aladdin (USA).sfc" / "Aladdin (USA).md"). A candidate whose sidecar entry names a
    // DIFFERENT ROM is declined — adopting it would bind this game to another game's save and the first
    // autosave would overwrite it, syncing as a plain Upload with no conflict and no notice. A candidate with
    // no recorded romPath (a pre-sidecar save) is adopted as before. An empty `romPath` argument therefore
    // declines every candidate the sidecar has a ROM for, which is the safe direction.
    QString resolvePath(const QString& root, const QString& systemId, const QString& base, const QString& ext,
                        const QString& romPath = QString());

    // One-time: move core-written save files that landed loose in the app directory into <data>/saves/.
    // LibretroCore::saveDir was never assigned, so cores that write their own files (memory cards, .brm,
    // .smpc) put them in the process working directory, where nothing backs them up. Moves ONLY the known
    // core-save extensions, never overwrites an existing file in saves/, treats a failed rename as a skip,
    // and logs every decision. Guarded by the "saves/straysSwept" setting.
    void sweepStrays();
}
