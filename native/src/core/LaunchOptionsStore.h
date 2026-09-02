// Per-game launch overrides (issue #51) — the one lever the app was missing: which core / which standalone
// emulator / with what extra args a SINGLE game launches, consulted before the system default so an empty
// override is byte-for-byte today's behaviour.
//
// WHY A NEW STORE, AND WHY IT LOOKS LIKE MetaOverrides. All emulator configuration was global: SystemCatalog
// picks a default core per system, EmulatorRegistry fixes each standalone emulator's argsTemplate, and the
// user's only lever was changing the default for a whole system. This store adds a per-GAME record — the one
// PS2 game that needs a renderer flag, the one SNES ROM that only runs on a different core. It is intent, not
// a cache, so it lives in the portable everythingbox.ini beside the other per-item stores (marks / favourites
// / metaoverrides), which is also the only place CloudMerge can carry it between devices.
//
// GLOBAL, not per profile (same posture as resume/* and metaoverrides/*, unlike marks/*): which binary a game
// runs on, and with what flag, is a property of the game+hardware, not of the viewer — a fix made on one
// profile is right for the whole household. So the layout is a flat hash, the same shape as metaoverrides:
//   launchopts/items/<md5(key)>  -> compact JSON blob { core, emulatorId, extraArgs, updatedAt }
//
// The item key is the SAME stable identity every other per-item store uses — the key GameLauncher::open()
// already receives (the catalog item id, else the file path) — hashed with the SAME full MD5-hex-over-UTF8 as
// ItemMarks/MetaOverrides. No new key scheme; overrides follow the game, not the path.
//
// FIELD SEMANTICS. A field is set when present and non-empty; an empty subset is "no override for that lever".
// There is exactly ONE spelling for "not overridden" (absent), never a choice between absent and "". Values
// are trimmed at write time so two devices that typed the same override with incidental whitespace store
// byte-identical records.
//
// WHICH LAUNCH PATH EACH FIELD AFFECTS (get this right — the pure resolvers below encode it):
//   * core       — the LIBRETRO path only: replaces CorePlan::core, but ONLY when it is one of the system's
//                  existing candidate cores (sys->cores). A stale/invalid override core (no longer a candidate)
//                  is ignored and the default stands. Libretro cores take no CLI args, so extraArgs is a no-op
//                  for them.
//   * emulatorId — the STANDALONE path only: replaces the resolved external-emulator id.
//   * extraArgs  — appended to the resolved argsTemplate of a STANDALONE emulator (meaningless for a core).
//   * contentUpdate / contentDlc (issue #189) — the STANDALONE path only: which update package (if any) and
//                  whether DLC is installed into the emulator's own content store before it boots. Read by
//                  ContentInstall::installForLaunch; a libretro core has no content store, so they are inert
//                  there. An unset pair is byte-for-byte today's behaviour (install what the sidecars hold).
//
// CLEAR IS A HUSK, NEVER A DELETION (issue #132, MetaOverrides' idiom verbatim). set() with an all-empty
// override on an item that HAD a record stores a timestamp-only husk {"updatedAt": now} rather than removing
// the row: a deletion is indistinguishable from "this device never saw that game", so the next merge with a
// peer still holding the old override would resurrect exactly what the user just cleared. The husk is newer,
// wins the merge, and propagates the clear. A husk reads as "no override" for every consumer. And a husk is
// only ever written where a record existed to clear — an all-empty write on an un-overridden game writes
// nothing (see set()). Husks are never compacted, for MetaOverrides' reason (a peer's stale copy has no
// expiry). A byte-equal write is a no-op that does NOT refresh the stamp (issue #167).
#pragma once
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <functional>
#include "EmuBackend.h"      // RetroPark Slice 2a: the backend the game launches on (resolveBackend's return)

namespace LaunchOpts
{
    struct Override
    {
        QString core;        // preferred libretro core base name (must be a system candidate to apply)
        QString emulatorId;  // preferred standalone-emulator id
        QString extraArgs;   // extra CLI args appended to a standalone emulator's resolved argsTemplate
        QString backend;     // RetroPark Slice 2a: "libretro"/"retropark" — which engine runs this game (empty = inherit)
        // ---- issue #189: the two content-install levers. Both are FULL levers (a game that sets only one is
        // a real record, not a husk), and both are STRINGS because each has THREE states and "absent" has to
        // be one of them — a bool would collapse "the default" and "on" into one value and lose exactly the
        // distinction this store is built on.
        QString contentUpdate; // "" = install whatever update sits in the sidecar; "none" = install no update
                               // (speedruns, mod setups, "that patch broke it"); anything else is a VERSION
                               // PIN, matched case-insensitively as a substring of the package's file name.
        QString contentDlc;    // "" / "on" = install the game's DLC; "off" = do not.
        qint64  updatedAt = 0;

        // No lever set. Ignores updatedAt, so a clear husk is empty (= "no override") while still being a real,
        // newer, propagating record.
        bool isEmpty() const;
    };

    // ---- pure: canonical record <-> JSON (trimmed + omit-empty -> ONE spelling per record) --------------------
    Override    fromJson(const QJsonObject& o);
    QJsonObject toJson(const Override& ov);
    Override    normalized(const Override& ov);   // trim every field (what set() stores)

    // ---- pure resolution — the mutation-tested heart (no ini, no disk) ---------------------------------------
    // The core to launch: ov.core when it is non-empty AND present in candidateCores; otherwise baseCore. An
    // override core that is not a candidate is stale/invalid and ignored, so the system default stands.
    QString resolveCore(const QString& baseCore, const Override& ov, const QStringList& candidateCores);
    // The standalone-emulator id to launch: ov.emulatorId when it is non-empty AND present in
    // validEmulatorIds; otherwise baseId. An override naming an emulator that has since been retired/removed
    // (no longer a registered id) is stale/invalid and ignored, so the system default stands — symmetric with
    // resolveCore's candidate check, and it stops a retired override from erroring the launch out.
    QString resolveEmulatorId(const QString& baseId, const Override& ov, const QStringList& validEmulatorIds);
    // The backend to launch on (RetroPark Slice 2a): ov.backend when it is a RECOGNISED backend string
    // ("libretro"/"retropark"); otherwise defaultBackend. An empty override inherits defaultBackend, and an
    // unknown/retired value falls back to it WITHOUT erroring — symmetric with resolveCore's non-candidate
    // check, so a stale sync or a spelling the app no longer offers can never refuse to launch a game. Does NOT
    // delegate to backendFromString(): that collapses unknown->Libretro, but the fallback here must be the
    // caller's default (which may itself be RetroPark).
    EmuBackend resolveBackend(EmuBackend defaultBackend, const Override& ov);
    // Append the user's extra args to a resolved args string, one space between, trimming the extra. A blank
    // extra is a byte-for-byte no-op (empty override == today's launch). No-op'ing here is what keeps the
    // libretro path — which never calls this — and an unset standalone override identical to today.
    QString appendExtraArgs(const QString& resolvedArgs, const QString& extra);
    // The resolved args STRING -> the argv list a standalone emulator is spawned with (issue #237). This is
    // EmulatorManager::launch's tokeniser, extracted so it can be pinned as a pure function: "resolved" is the
    // template with {fs} already substituted and any extra args already appended, "romNative" the ROM path in
    // the platform's native separators.
    //
    // SPLIT FIRST, SUBSTITUTE AFTER, PER TOKEN. That ordering is why a ROM path containing spaces has always
    // worked without quoting: {rom} lands INSIDE one already-cut token, so its spaces can never become
    // separators and no quote characters are ever baked into the argument. Every placeholder must keep being
    // substituted here, after the cut, for that to hold.
    //
    // The cut itself is QProcess::splitCommand - Qt's own shell-style tokeniser. A token containing a space is
    // written in double quotes (--config "My Profile" is TWO arguments), three consecutive double quotes are a
    // literal quote, and - load-bearing on Windows - a backslash is NOT an escape, so a quoted
    // "C:\Program Files\..." survives with its backslashes intact. A template with no double quote in it
    // tokenises byte-for-byte as the plain space-split this replaced.
    //
    // An empty token is dropped, which is what makes a blank {fs} (a windowed launch) and a blank {rom}
    // (opening an emulator's own UI with no game) collapse away instead of becoming an empty argv element.
    QStringList buildArgs(const QString& resolved, const QString& romNative);

    // ---- store (global; husk-on-clear; QtCore-only, same posture as MetaOverrides) ---------------------------
    QString  hashKey(const QString& key);   // md5-hex of the UTF-8 key (ItemMarks/MetaOverrides scheme)
    Override get(const QString& key);       // absent/empty key -> a default (all-clear) Override
    bool     has(const QString& key);       // is any lever set for this game
    void     set(const QString& key, const Override& ov); // normalize; stamp+persist only on a real change; husk-on-clear
    void     reset(const QString& key);     // clear every lever: a newer, empty, still-propagating husk

    void invalidate();                      // drop the cache (external ini change / after a cloud merge)

    // Multi-device sync trigger, same contract as ItemMarks/MetaOverrides::setChangeHook: a std::function fired
    // after every mutation so MainWindow can (re)arm the debounced push. Unset in headless probes (fires nothing).
    void setChangeHook(std::function<void()> hook);
}
