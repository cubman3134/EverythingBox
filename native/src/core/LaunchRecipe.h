// LAUNCH RECIPES (issue #190) — the folklore a retro COMPUTER needs, as data.
//
// A console is "insert cartridge, press play". A retro computer is not: it wants a machine model, a memory
// size, firmware the user must supply, content presented in a particular shape, and often a typed command.
// SystemCatalog (#92) already says WHICH core runs a system; it says nothing about HOW that core has to be
// set up for the system to actually boot. That knowledge existed only in the user's head — and, for exactly
// one system, in a hardcoded C++ table (RetroView's `kSeeds`, which pinned hatari's TOS option). This header
// is that knowledge, moved into files:
//
//     native/systems/recipes/<systemId>.json      shipped, embedded via native/resources/recipes.qrc
//     <data>/systems/recipes/<systemId>.json      the user's override, merged field-by-field over the shipped one
//
// exactly the way <data>/systems/*.json overrides SystemCatalog and <data>/ports/*.json overrides
// NativePorts. Adding a system's launch knowledge — or correcting a core option after an upstream rename —
// is a file, not a rebuild.
//
// WHAT A RECIPE SAYS, and why each part is here rather than in C++:
//   * per core: the core OPTIONS to seed (machine model, memory, video standard). Per CORE and not per
//     system, because a system has several candidate cores and `puae_model` means nothing to `puae2021`'s
//     successor — a recipe that seeded options by system id would push a key at whichever core happened to
//     be selected. A `"core": "*"` entry is the any-core fallback.
//   * per core: the FIRMWARE it needs, by filename + purpose. Firmware is the user's (Kickstart is sold, not
//     downloaded — the line BiosCatalog already draws), so the app's whole job is to say precisely which file
//     is missing and which folder it goes in. That message is built here, from data, so it can name the file.
//   * per core: how CONTENT is presented. The same .lha is a WHDLoad archive to puae and a nonsense blob to
//     anything else; a DOS game is a folder to mount, and dosbox-pure wants the executable path inside it.
//   * per core: an optional BOOT COMMAND, for the cores that take one.
//   * per system: how to AUTODETECT an executable inside a game folder, and which names to prefer or avoid.
//
// EVERYTHING HERE IS PURE except the four accessors at the bottom (QtCore file/resource reads, no app state).
// Firmware presence is asked through an injected `exists` predicate rather than touching the disk, and the
// executable pick takes a list of names rather than scanning a directory — so probe_recipes drives every rule
// against literals with no filesystem in the way, the discipline SystemCatalog and NativePorts follow.
//
// A MALFORMED RECIPE IS AN ERROR, NOT AN EMPTY ONE. parse() returns false with a reason. The loader logs it
// and falls back to the shipped file (or to no recipe at all), which is the pre-#190 behaviour — a recipe can
// never be the reason a launch that used to work stops working. That is the only safety property this file
// needs, and it is why `forSystem` returning a null recipe has to remain a completely ordinary outcome:
// every consumer treats "no recipe" as "do exactly what the app did before".
#pragma once
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <functional>

#include "AppPaths.h"

// ---- the schema ---------------------------------------------------------------------------------------

// One firmware requirement. `files` is an ANY-OF list: a Kickstart 3.1 is `kick40068.A1200` under puae's own
// short-name convention AND a long TOSEC ".rom" name, and either satisfies the requirement — so the check is
// "none of these are present", never "the first one is absent".
struct RecipeFirmware
{
    QString     purpose;            // human phrase for the message: "Kickstart 3.1 (A1200)"
    QStringList files;              // acceptable file names; ANY present satisfies the requirement
    QStringList md5;                // optional known digests, carried for a later hash check (not enforced here)
    bool        required = true;    // false => the core still boots without it (an optional extra)
};

// What the user handed us. Decided by the caller (it has the path), never guessed here.
enum class ContentKind { File, Folder, Archive };

// How the resolved content must reach the core.
//   AsIs        — hand the path straight to the core. For an ARCHIVE this is load-bearing and is NOT the
//                 app's default: today every archive is extracted to a temp file before launch, which is
//                 exactly wrong for dosbox-pure (it reads ZIPs natively and writes its modifications into a
//                 save file beside them) and for puae (a WHDLoad .lha is the content).
//   Executable  — find the program inside a folder and hand the core THAT path.
//   Extract     — extract the archive to a folder and hand the core the folder.
//   Unknown     — the recipe said NOTHING about this shape, or said something we cannot read. Callers map
//                 Unknown onto what the app did before #190, which is per shape: a file and a folder are
//                 handed over as they are, an ARCHIVE is extracted. Silence therefore never changes
//                 behaviour, and a typo in a data file degrades to today's launch instead of erroring it.
//                 (This is why Unknown exists rather than defaulting to AsIs: AsIs on an archive is a real,
//                 different behaviour — the thing the MS-DOS recipe turns ON deliberately — so "unstated"
//                 and "stated as asIs" must not be the same value.)
enum class Presentation { AsIs, Executable, Extract, Unknown };

struct RecipeContentRule
{
    ContentKind  when = ContentKind::File;
    Presentation present = Presentation::AsIs;
};

struct RecipeCore
{
    QString                core;        // libretro core base name, or "*" for any core of this system
    QMap<QString, QString> options;     // core-option key -> value, seeded only where the user set nothing
    QList<RecipeFirmware>  firmware;
    QList<RecipeContentRule> content;
    QString                bootCommand; // optional command the core is asked to run after boot ("" = none)
};

// The executable-autodetect rules for a folder game (MS-DOS is the case that needs them).
struct RecipeExecutables
{
    QStringList extensions;  // lowercase, no dot: the formats that count as a program ("bat","exe","com")
    QStringList prefer;      // lowercase base names promoted ahead of the rest ("start","play")
    QStringList avoid;       // lowercase base names demoted behind the rest ("install","setup")
};

struct LaunchRecipe
{
    QString systemId;
    QString summary;                    // one line, shown in the docs + the system folder README
    QString firmwareFolder;             // where firmware goes, as the USER reads it: "system"
    bool    folderIsGame = false;       // a sub-folder holding a program is ONE library entry, not N files
    QList<RecipeCore>  cores;
    RecipeExecutables  executables;

    bool isNull() const { return systemId.isEmpty(); }
};

namespace LaunchRecipes
{
    // ---- pure: enum <-> wire spelling -------------------------------------------------------------------
    inline ContentKind contentKindFromString(const QString& s)
    {
        if (s.compare(QLatin1String("folder"), Qt::CaseInsensitive) == 0)  return ContentKind::Folder;
        if (s.compare(QLatin1String("archive"), Qt::CaseInsensitive) == 0) return ContentKind::Archive;
        return ContentKind::File;
    }
    inline QString contentKindToString(ContentKind k)
    {
        switch (k) { case ContentKind::Folder: return QStringLiteral("folder");
                     case ContentKind::Archive: return QStringLiteral("archive");
                     default: return QStringLiteral("file"); }
    }
    // Deliberately a whitelist: an unknown spelling is Unknown, never silently AsIs at THIS level — the
    // caller decides that a rule it cannot read means "behave as before", and the probe can see the
    // difference between "the file said asIs" and "the file said something we don't understand".
    inline Presentation presentationFromString(const QString& s)
    {
        if (s.compare(QLatin1String("asIs"), Qt::CaseInsensitive) == 0)       return Presentation::AsIs;
        if (s.compare(QLatin1String("executable"), Qt::CaseInsensitive) == 0) return Presentation::Executable;
        if (s.compare(QLatin1String("extract"), Qt::CaseInsensitive) == 0)    return Presentation::Extract;
        return Presentation::Unknown;
    }
    inline QString presentationToString(Presentation p)
    {
        switch (p) { case Presentation::Executable: return QStringLiteral("executable");
                     case Presentation::Extract:    return QStringLiteral("extract");
                     case Presentation::Unknown:    return QStringLiteral("unknown");
                     default:                       return QStringLiteral("asIs"); }
    }

    inline QStringList strList(const QJsonValue& v, bool lower)
    {
        QStringList out;
        if (!v.isArray()) return out;
        for (const QJsonValue& e : v.toArray())
        {
            if (!e.isString()) continue;
            const QString s = lower ? e.toString().trimmed().toLower() : e.toString().trimmed();
            if (!s.isEmpty()) out.push_back(s);
        }
        return out;
    }

    // ---- pure: parse ------------------------------------------------------------------------------------
    // Bytes -> recipe. False (with a reason in *err) for: unparseable JSON, a non-object document, and a
    // document with no "system". Everything else is tolerated field-by-field — a recipe that names one core
    // wrongly must not cost the user the rest of the file.
    inline bool parse(const QByteArray& bytes, LaunchRecipe* out, QString* err)
    {
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
        if (pe.error != QJsonParseError::NoError)
        {
            if (err) *err = QStringLiteral("not valid JSON (%1 at offset %2)").arg(pe.errorString()).arg(pe.offset);
            return false;
        }
        if (!doc.isObject())
        {
            if (err) *err = QStringLiteral("the document is not a JSON object");
            return false;
        }
        const QJsonObject o = doc.object();
        LaunchRecipe r;
        r.systemId = o.value(QStringLiteral("system")).toString().trimmed();
        if (r.systemId.isEmpty())
        {
            if (err) *err = QStringLiteral("no \"system\" field");
            return false;
        }
        r.summary        = o.value(QStringLiteral("summary")).toString().trimmed();
        r.firmwareFolder = o.value(QStringLiteral("firmwareFolder")).toString().trimmed();
        r.folderIsGame   = o.value(QStringLiteral("folderIsGame")).toBool(false);

        const QJsonObject ex = o.value(QStringLiteral("executables")).toObject();
        r.executables.extensions = strList(ex.value(QStringLiteral("extensions")), true);
        r.executables.prefer     = strList(ex.value(QStringLiteral("prefer")), true);
        r.executables.avoid      = strList(ex.value(QStringLiteral("avoid")), true);

        for (const QJsonValue& cv : o.value(QStringLiteral("cores")).toArray())
        {
            if (!cv.isObject()) continue;
            const QJsonObject co = cv.toObject();
            RecipeCore rc;
            rc.core = co.value(QStringLiteral("core")).toString().trimmed();
            if (rc.core.isEmpty()) continue;   // a core entry that names no core can never be selected
            rc.bootCommand = co.value(QStringLiteral("bootCommand")).toString();

            const QJsonObject opts = co.value(QStringLiteral("options")).toObject();
            for (auto it = opts.constBegin(); it != opts.constEnd(); ++it)
            {
                const QString k = it.key().trimmed();
                // Values are written as strings because that is what libretro core options ARE; a number in
                // the file is accepted and stringified so "memory_size": 16 is not a silent no-op.
                QString v;
                if (it.value().isString())      v = it.value().toString().trimmed();
                else if (it.value().isDouble()) v = QString::number(it.value().toDouble(), 'g', 17);
                else if (it.value().isBool())   v = it.value().toBool() ? QStringLiteral("true") : QStringLiteral("false");
                else continue;
                if (!k.isEmpty() && !v.isEmpty()) rc.options.insert(k, v);
            }

            for (const QJsonValue& fv : co.value(QStringLiteral("firmware")).toArray())
            {
                if (!fv.isObject()) continue;
                const QJsonObject fo = fv.toObject();
                RecipeFirmware fw;
                fw.purpose  = fo.value(QStringLiteral("purpose")).toString().trimmed();
                fw.files    = strList(fo.value(QStringLiteral("files")), false);
                fw.md5      = strList(fo.value(QStringLiteral("md5")), true);
                fw.required = fo.value(QStringLiteral("required")).toBool(true);
                if (fw.files.isEmpty()) continue;   // a requirement that names no file cannot be checked
                rc.firmware.push_back(fw);
            }

            for (const QJsonValue& nv : co.value(QStringLiteral("content")).toArray())
            {
                if (!nv.isObject()) continue;
                const QJsonObject no = nv.toObject();
                RecipeContentRule rule;
                rule.when    = contentKindFromString(no.value(QStringLiteral("when")).toString());
                rule.present = presentationFromString(no.value(QStringLiteral("present")).toString());
                rc.content.push_back(rule);
            }
            r.cores.push_back(rc);
        }
        if (out) *out = r;
        return true;
    }

    // ---- pure: lookups ----------------------------------------------------------------------------------
    // The core entry that applies to `coreName`: an exact match first, then the "*" any-core entry. Null when
    // the recipe says nothing about this core, which every consumer reads as "behave as before".
    inline const RecipeCore* coreFor(const LaunchRecipe& r, const QString& coreName)
    {
        for (const RecipeCore& c : r.cores) if (c.core == coreName) return &c;
        for (const RecipeCore& c : r.cores) if (c.core == QLatin1String("*")) return &c;
        return nullptr;
    }

    // How this core wants content of this shape presented. A recipe that says NOTHING about a shape yields
    // Unknown, not AsIs, and the caller maps that onto its own pre-#190 behaviour (see the enum). Saying
    // nothing must never be the same as saying "asIs", because on an archive those are opposite actions.
    inline Presentation presentationFor(const RecipeCore& c, ContentKind kind)
    {
        for (const RecipeContentRule& rule : c.content) if (rule.when == kind) return rule.present;
        return Presentation::Unknown;
    }

    // ---- pure: firmware -----------------------------------------------------------------------------------
    // The REQUIRED firmware entries none of whose files are present. `exists` is injected (given a bare file
    // name, is it in the firmware folder?), so this is pure and the probe drives it against a set of literals.
    // Optional entries are never reported: a missing extra is not a reason to refuse a launch.
    inline QList<RecipeFirmware> missingFirmware(const RecipeCore& c,
                                                 const std::function<bool(const QString&)>& exists)
    {
        QList<RecipeFirmware> missing;
        if (!exists) return missing;
        for (const RecipeFirmware& fw : c.firmware)
        {
            if (!fw.required) continue;
            bool have = false;
            for (const QString& f : fw.files) if (exists(f)) { have = true; break; }
            if (!have) missing.push_back(fw);
        }
        return missing;
    }

    // THE MESSAGE. The whole point of the firmware half of #190: never a silent black screen, never a
    // download — a sentence that names the exact file and the exact folder. Built here (not at the call site)
    // so probe_recipes can pin the wording, and so it reads the same on every surface.
    //
    // Shape, for one missing entry:
    //   “Lemmings” needs the Amiga Kickstart 1.3 (A500): put kick34005.A500 in the system folder
    //   (C:/…/system). EverythingBox can't download it — Kickstart is copyrighted.
    // With several acceptable names, the FIRST is named and the rest are offered as alternatives, because a
    // list of eight TOSEC spellings is not an instruction.
    inline QString firmwareMessage(const QString& gameTitle, const QString& systemName,
                                   const QList<RecipeFirmware>& missing, const QString& folderPath)
    {
        if (missing.isEmpty()) return QString();
        QStringList parts;
        for (const RecipeFirmware& fw : missing)
        {
            const QString purpose = fw.purpose.isEmpty() ? systemName : fw.purpose;
            QString one = QStringLiteral("%1: put %2 in the system folder (%3)")
                              .arg(purpose, fw.files.value(0), folderPath);
            if (fw.files.size() > 1)
                one += QStringLiteral(" — or any of: %1").arg(QStringList(fw.files.mid(1)).join(QStringLiteral(", ")));
            parts.push_back(one);
        }
        const QString head = gameTitle.trimmed().isEmpty()
                                 ? QStringLiteral("%1 needs firmware you have to supply").arg(systemName)
                                 : QStringLiteral("“%1” needs firmware you have to supply").arg(gameTitle.trimmed());
        return head + QStringLiteral(". ") + parts.join(QStringLiteral("; "))
             + QStringLiteral(". EverythingBox can't download it for you — this firmware is copyrighted.");
    }

    // ---- pure: executable autodetect ----------------------------------------------------------------------
    // The result of looking inside a DOS game folder. Exactly one of these is true:
    //   * `chosen` non-empty            — one program won outright; launch it, no question asked;
    //   * `candidates` size > 1         — genuinely ambiguous; the caller offers the picker and REMEMBERS it;
    //   * both empty                    — nothing that looks like a program; the caller falls back (hand the
    //                                     folder itself to the core and let the core's own start menu deal).
    struct ExecutablePick
    {
        QString     chosen;
        QStringList candidates;   // relative paths, in the order to offer them
        bool ambiguous() const { return chosen.isEmpty() && candidates.size() > 1; }
    };

    // Base name of a relative path, lowercased and without its extension ("SUB/START.BAT" -> "start").
    inline QString execBaseName(const QString& relPath)
    {
        QString s = relPath;
        s.replace(QLatin1Char('\\'), QLatin1Char('/'));
        const int slash = s.lastIndexOf(QLatin1Char('/'));
        if (slash >= 0) s = s.mid(slash + 1);
        const int dot = s.lastIndexOf(QLatin1Char('.'));
        if (dot > 0) s = s.left(dot);
        return s.toLower();
    }
    inline QString execExtension(const QString& relPath)
    {
        const int dot = relPath.lastIndexOf(QLatin1Char('.'));
        if (dot < 0) return QString();
        return relPath.mid(dot + 1).toLower();
    }
    // How deep a relative path sits (0 = directly in the game folder). A program at the top of the folder is
    // the game's launcher far more often than one three directories down, so depth breaks ties before
    // anything else does.
    inline int execDepth(const QString& relPath)
    {
        QString s = relPath; s.replace(QLatin1Char('\\'), QLatin1Char('/'));
        return int(s.count(QLatin1Char('/')));
    }

    // THE DOS AUTODETECT RULE, in the order #190 fixes it. `relPaths` is every file under the game folder
    // (relative, any order); `folderName` is the folder's own name; `rules` comes from the recipe.
    //
    //   0. keep only files whose extension the recipe calls a program (.bat/.exe/.com for DOS);
    //   1. keep only the SHALLOWEST such files — a launcher at the top beats one inside INSTALL\;
    //   2. if the recipe PREFERS a name and one is present, that set wins outright;
    //   3. drop the recipe's AVOID names (INSTALL/SETUP/…) — but only while something survives, because a
    //      folder whose only program is INSTALL.EXE still has to be launchable;
    //   4. one left  -> that is the answer;
    //   5. prefer a .BAT (the DOS convention: the batch file is what sets up and calls the real binary);
    //   6. else prefer the program whose name matches the FOLDER (DOOM\DOOM.EXE);
    //   7. else it is genuinely ambiguous — hand the candidates back for the picker.
    // Order is deliberate and each step is pinned by probe_recipes, including the ambiguous case.
    inline ExecutablePick chooseExecutable(const QStringList& relPaths, const QString& folderName,
                                           const RecipeExecutables& rules)
    {
        ExecutablePick pick;
        if (rules.extensions.isEmpty()) return pick;

        // 0 — programs only.
        QStringList progs;
        for (const QString& p : relPaths)
            if (rules.extensions.contains(execExtension(p))) progs.push_back(p);
        if (progs.isEmpty()) return pick;

        // 1 — shallowest wins.
        int best = execDepth(progs.first());
        for (const QString& p : progs) best = qMin(best, execDepth(p));
        QStringList shallow;
        for (const QString& p : progs) if (execDepth(p) == best) shallow.push_back(p);
        progs = shallow;

        // 2 — an explicitly preferred name wins outright.
        if (!rules.prefer.isEmpty())
        {
            QStringList liked;
            for (const QString& p : progs) if (rules.prefer.contains(execBaseName(p))) liked.push_back(p);
            if (!liked.isEmpty()) progs = liked;
        }

        // 3 — demote installers, but never to nothing.
        if (!rules.avoid.isEmpty())
        {
            QStringList kept;
            for (const QString& p : progs) if (!rules.avoid.contains(execBaseName(p))) kept.push_back(p);
            if (!kept.isEmpty()) progs = kept;
        }

        std::sort(progs.begin(), progs.end(),
                  [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });

        if (progs.size() == 1) { pick.chosen = progs.first(); return pick; }  // 4

        // 5 — a .BAT is the DOS launcher convention.
        QStringList bats;
        for (const QString& p : progs) if (execExtension(p) == QLatin1String("bat")) bats.push_back(p);
        if (bats.size() == 1) { pick.chosen = bats.first(); return pick; }
        if (bats.size() > 1) progs = bats;

        // 6 — the program named after the folder.
        const QString folderKey = folderName.trimmed().toLower();
        if (!folderKey.isEmpty())
        {
            QStringList named;
            for (const QString& p : progs) if (execBaseName(p) == folderKey) named.push_back(p);
            if (named.size() == 1) { pick.chosen = named.first(); return pick; }
        }

        pick.candidates = progs;   // 7 — the picker's job
        return pick;
    }

    // ---- pure: is this folder one game? -------------------------------------------------------------------
    // For a system whose recipe sets folderIsGame, a sub-folder of the system's ROM folder is ONE library
    // entry when it holds at least one program. Without this, a DOS game folder scatters into a tile per
    // file — the "extracted mess" #190 names — and the folder itself is unreachable, because the library
    // lists files.
    inline bool folderLooksLikeGame(const LaunchRecipe& r, const QStringList& relPaths)
    {
        if (!r.folderIsGame || r.executables.extensions.isEmpty()) return false;
        for (const QString& p : relPaths)
            if (r.executables.extensions.contains(execExtension(p))) return true;
        return false;
    }

    // ---- loading (QtCore only) ----------------------------------------------------------------------------
    // The resource a system's shipped recipe is embedded at. Named ONCE here so the app, the probe and the
    // .qrc alias cannot drift — probe_recipes asserts this resource exists, parses, and is byte-identical to
    // the file in the source tree, which is the only way to catch a .qrc that reached the target the wrong
    // way (qt_add_executable's initial source list embeds nothing, silently — see native/CMakeLists.txt).
    inline QString shippedResource(const QString& systemId)
    {
        return QStringLiteral(":/recipes/%1.json").arg(systemId);
    }

    // <data>/systems/recipes — the user's overrides, beside <data>/systems (#92).
    inline QString dataRecipesDir() { return AppPaths::dataDir() + QStringLiteral("/systems/recipes"); }

    // Read ONE recipe: the user's file if it is there and parses, else the shipped one. A user file that
    // fails to parse is reported and IGNORED — the shipped recipe stands, so a typo costs the user their
    // customisation and nothing else. Returns a null recipe when neither exists (an ordinary outcome).
    inline LaunchRecipe load(const QString& systemId, const QString& dataDir,
                             const std::function<void(const QString&)>& warn = {})
    {
        auto readFile = [](const QString& path, LaunchRecipe* out, QString* err) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) { if (err) *err = QStringLiteral("cannot open"); return false; }
            const QByteArray bytes = f.readAll();
            f.close();
            return parse(bytes, out, err);
        };

        LaunchRecipe r;
        if (!dataDir.isEmpty())
        {
            const QString user = dataDir + QStringLiteral("/") + systemId + QStringLiteral(".json");
            if (QFileInfo::exists(user))
            {
                QString err;
                if (readFile(user, &r, &err)) return r;
                if (warn) warn(QStringLiteral("%1.json: %2 — using the shipped recipe").arg(systemId, err));
                r = LaunchRecipe{};
            }
        }
        QString err;
        if (readFile(shippedResource(systemId), &r, &err))
            return r;
        if (warn && !err.isEmpty() && err != QLatin1String("cannot open"))
            warn(QStringLiteral("%1.json (shipped): %2").arg(systemId, err));
        return LaunchRecipe{};
    }

    // The app's accessor: this system's recipe, cached for the process (a new data file needs a restart,
    // like every other catalog here). A system with no recipe caches a null one, so the miss costs one
    // resource probe, not one per launch.
    inline const LaunchRecipe& forSystem(const QString& systemId)
    {
        static QMap<QString, LaunchRecipe> cache;
        auto it = cache.constFind(systemId);
        if (it != cache.constEnd()) return *it;
        const LaunchRecipe r = load(systemId, dataRecipesDir(),
            [systemId](const QString& m) { qWarning("LaunchRecipes: %s", qUtf8Printable(m)); });
        return *cache.insert(systemId, r);
    }
}
