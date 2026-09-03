// Installing a game's UPDATE and DLC packages into the target emulator before it boots (issue #189) — the
// machinery over ContentRecipe.h's schema: where the packages come from, what has already been installed, and
// the two appliers increment 1 wires (Ryujinx's jsonRegistry, Cemu's copyTree).
//
// THE SIDECAR CONVENTION, which is the whole of increment 1's sourcing:
//
//     <ROMs>/Wii U/Some Game [0005000010101C00].wux
//     <ROMs>/Wii U/updates/<anything>          <- installed as an UPDATE
//     <ROMs>/Wii U/dlc/<anything>              <- installed as DLC
//
// `updates/` and `dlc/` sit BESIDE the game file, and their DIRECT CHILDREN (a file or a folder — Cemu update
// content is a folder of code/content/meta) are the packages. Nothing is recursed into at the discovery
// level: one child is one package, which is the only rule a user can hold in their head, and it is the rule
// every one of these emulators' own "install this" gesture already follows. Server-served attachments are
// increment 2 and land as another source feeding the SAME plan.
//
// IDEMPOTENT, AND TRACKED BY HASH, NEVER BY PRESENCE. Re-copying a 4 GB package on every launch is slow and
// is a corruption risk, and "the destination exists" is not evidence WE put it there. So a device-local
// record (<data>/contentinstall/<emulatorId>.json) holds, per (emulator, title id, package): the package's
// sha1, its size+mtime stamp, and where it went. A launch installs only what that record does not already
// claim. The stamp is the FAST gate (a match skips without reading the file at all — HashVerify's own
// path+mtime+size idiom); the sha1 is the AUTHORITY, so a package that was merely touched or moved is
// recognised and not reinstalled, and a package whose bytes changed is.
//
// AND THE RECORD IS THE AUTHORITY IN BOTH DIRECTIONS. A package this device has recorded is NOT reinstalled
// even when it is no longer at its destination — because "no longer there" is usually the user having removed
// it in the emulator's own UI, and a frontend that silently puts it back on every launch is exactly the
// fighting-the-user failure #189 exists to avoid. Clearing the record (or the app's data dir) is what asks for
// a reinstall. probe_contentinstall pins this by deleting the installed content and asserting the next launch
// leaves it deleted.
//
// RESTRAINT. Before the FIRST write for a (title, slot) the emulator's own content index is SNAPSHOTTED into
// the record — the registry file's bytes for jsonRegistry, the destination tree's listing for copyTree — so
// what was there before this app ever touched it is recoverable and stateable. Every individual write then
// goes through ContentRecipe::verdictForFile / verdictForScalar: content that is already there and is NOT
// ours is left alone and reported. There is no flag that turns that off.
//
// A FAILURE IS A REPORT, NEVER A REFUSAL. installForLaunch returns what happened and the caller logs it; the
// game launches either way, exactly as the PS3 chain does. Nothing here can stop a boot.
#pragma once
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include "ContentRecipe.h"

namespace ContentInstall
{
    using ContentRecipe::Recipe;
    using ContentRecipe::Spec;
    using ContentRecipe::Vars;
    using ContentRecipe::Verdict;

    // ---- the record ---------------------------------------------------------------------------------------
    // One package this device has installed for one title. `dest` is the file (or directory) it landed at, and
    // is what makes a later overwrite OURS rather than a clobber.
    struct Item
    {
        QString slot;     // "updates" | "dlc"
        QString name;     // the package's base name (its identity to the user)
        QString sha1;     // payload sha1 of the package file (empty for a folder package — see treeSha1)
        QString dest;     // absolute destination path we wrote
        qint64  size = 0; // source size at install time  } together, the FAST stamp that skips a re-hash
        qint64  mtime = 0;// source mtime (epoch seconds)  }
        qint64  at = 0;   // when we installed it (epoch seconds)
        // For a FOLDER package: the paths, relative to `dest`, that WE actually laid down. This is what makes
        // ownership per-FILE rather than per-directory, and it is load-bearing: a user's own file sitting in
        // the same destination folder must not become "ours" merely because we installed a package around it,
        // or the next (newer) package would clobber it. Empty for a single-file package, where `dest` is the
        // file itself and exact equality is the whole test.
        QStringList files;
    };

    struct TitleRecord
    {
        QVector<Item> items;
        QJsonObject   snapshots;   // slot -> the pre-first-write snapshot of the emulator's content index
        bool isEmpty() const { return items.isEmpty() && snapshots.isEmpty(); }
    };

    struct Record
    {
        QHash<QString, TitleRecord> titles;   // by upper-cased title id
    };

    // ---- pure: what the record says -----------------------------------------------------------------------
    // Has this exact package already been installed for this title+slot? The stamp (name+size+mtime) is
    // checked first and is sufficient; a `sha1` is consulted only when it is non-empty, which is what lets the
    // caller skip hashing a package whose stamp already matches.
    bool alreadyInstalled(const TitleRecord& rec, const QString& slot, const QString& name,
                          qint64 size, qint64 mtime, const QString& sha1);
    // Does the record claim we wrote the file currently at `destFile`? (Case-insensitive, path-normalised.)
    // For a folder package this is true only for the individual files Item::files names — NOT for everything
    // under the destination directory. See Item::files.
    bool weInstalled(const TitleRecord& rec, const QString& destFile);
    // Every destination this app recorded for a slot — the "ours" set verdictForScalar consults.
    QStringList ourPaths(const TitleRecord& rec, const QString& slot);

    // ---- pure: JSON <-> record ----------------------------------------------------------------------------
    QJsonObject toJson(const Record& r);
    Record      fromJson(const QJsonObject& o);

    // ---- the record's file (device-local; NOT synced — it describes THIS machine's emulator install) -------
    QString recordDir();
    QString recordPath(const QString& emulatorId);
    Record  loadRecord(const QString& emulatorId);
    bool    saveRecord(const QString& emulatorId, const Record& r);

    // ---- sidecar discovery --------------------------------------------------------------------------------
    // <game folder>/<slot>. Returns the path whether or not it exists (callers test existence).
    QString sidecarDir(const QString& gamePath, const QString& slot);
    // The DIRECT children of that folder, sorted by name — one child is one package. Missing folder -> empty.
    QStringList discover(const QString& gamePath, const QString& slot);
    // The BASE game's title id: an explicit <game folder>/titleid.txt wins (the owner's escape hatch for a
    // dump whose name says nothing), else the game file's own name, else its folder's name. Upper case, or
    // empty — and empty is a REPORT, not a guess, because a wrong id writes into another game's content.
    QString resolveTitleId(const QString& gamePath);

    // ---- pure: the plan -----------------------------------------------------------------------------------
    enum class Decision
    {
        Install,           // do it
        AlreadyInstalled,  // the record already claims this exact package
        PinnedOut,         // the per-game version pin does not accept this package
        Disabled,          // the per-game lever says no (update pin "none", or DLC off)
        NoRecipe,          // this emulator declares nothing for this slot
        UnknownKind,       // a recipe kind this build does not know — ignored, logged once
        Delegated,         // "emulatorUpdater": the emulator owns this conversation
        DescribedOnly,     // "cli": described here, performed by the emulator's own existing code path
        NoTitleId          // the recipe needs a title id and none could be derived
    };

    // A candidate package as the planner sees it. `sha1` may be empty on the first pass (see the two-pass note
    // on planSlot) — that is the whole point of the size+mtime stamp.
    struct Candidate
    {
        QString path;
        QString name;
        qint64  size = 0;
        qint64  mtime = 0;
        QString sha1;
        bool    isDir = false;
    };

    struct Planned
    {
        Candidate item;
        QString   slot;
        Decision  decision = Decision::NoRecipe;
        QString   note;
    };

    // PURE. No disk, no clock. Two-pass by design: call it once with unhashed candidates (stamp gate only),
    // hash exactly the ones that came back Install, then call it again with those hashes filled in — so a
    // library whose packages are all already installed costs zero bytes read.
    QVector<Planned> planSlot(const Recipe& recipe, const QString& slot, const QVector<Candidate>& candidates,
                              const TitleRecord& rec, const QString& lever, const QString& titleId);

    // ---- pure: merging an entry into a jsonRegistry index --------------------------------------------------
    struct MergeResult
    {
        QJsonDocument doc;
        bool          changed = false;
        QStringList   leftAlone;   // keys whose existing value belongs to the user and was not touched
    };
    // `container` is "array" or "object" (empty = "object"). Existing content is PRESERVED: an array keeps
    // every element it had, in order, and gains the entry only if no element already names the same file; an
    // object keeps every key it had, array-valued keys gain missing elements, and a scalar key is rewritten
    // only when ContentRecipe::verdictForScalar allows it.
    MergeResult mergeRegistry(const QJsonDocument& existing, const QString& container,
                              const QJsonObject& entry, const QStringList& oursPaths);

    // Does any string anywhere inside `v` equal `needle` (path-normalised, case-insensitive)? The identity
    // test for "this index already names that package".
    bool jsonNamesPath(const QJsonValue& v, const QString& needle);

    // ---- glue: hashing ------------------------------------------------------------------------------------
    // Streaming sha1 of a file's bytes — byte-identical to HashVerify::hashBytes(<the file>).sha1 (#97's
    // hash), computed without reading a multi-gigabyte package into memory. probe_contentinstall pins that
    // equivalence against HashVerify itself. Empty on an unreadable file.
    QString fileSha1(const QString& path);
    // A folder package's identity: the sha1 over its sorted (relative path, size, sha1) listing, so a Cemu
    // update FOLDER has a stable hash the record can gate on exactly like a single file.
    QString treeSha1(const QString& dir);

    // ---- glue: the snapshot (taken ONCE, before the first write for a title+slot) --------------------------
    QJsonObject snapshotOfFile(const QString& path);   // {"kind":"file","present":bool,"bytes":<base64>}
    QJsonObject snapshotOfTree(const QString& dir);    // {"kind":"tree","present":bool,"files":[{p,n}...]}

    // ---- the launch-time entry point ----------------------------------------------------------------------
    enum class Outcome { Installed, AlreadyInstalled, LeftAlone, Skipped, Failed };

    struct ItemResult
    {
        QString slot, name, source, dest, note;
        Outcome outcome = Outcome::Skipped;
    };

    struct Result
    {
        QVector<ItemResult> items;
        QStringList         log;     // one line per decision worth stating, for the launch log
        int installed = 0, skipped = 0, leftAlone = 0, failed = 0;
        bool didAnything() const { return installed > 0; }
    };

    // Resolve {data} for this emulator: the FIRST of `spec.dataDirs` that exists, else the first entry (which
    // is created on demand). Templates are expanded with `vars`.
    QString resolveDataDir(const Spec& spec, const Vars& vars);
    // The per-user application-data root ({appData}).
    QString appDataRoot();

    // THE ENTRY POINT the launch path calls. Never throws, never blocks a boot, never returns a failure the
    // caller has to act on: the Result is a REPORT. `updateLever` / `dlcLever` are the #51 per-game override
    // strings ("" = default, "none" / "off" = do not install, anything else = a version pin).
    Result installForLaunch(const QString& emulatorId, const Spec& spec, const QString& gamePath,
                            const QString& emuDir, const QString& updateLever, const QString& dlcLever);
}
