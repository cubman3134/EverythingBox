// Per-emulator CONTENT-INSTALL RECIPES as data (issue #189) — how a given standalone emulator ingests a game
// UPDATE or a DLC package, described in the emulator registry's own JSON schema (#52) rather than in code.
//
// WHY DATA. PS3 already had this end to end (native/src/core/ps3/ — fetch Sony's update chain, hand each PKG
// to `rpcs3 --headless --installpkg`, verify against the PKG's own entry table), and every line of it is
// RPCS3-shaped. Switch, Wii U, 3DS and Vita each ingest content a DIFFERENT way — a JSON file edit, a copy
// into a title path, an in-app flow — so generalising by writing four more RPCS3-shaped modules would be four
// more places to break. What actually differs between the emulators is a HANDFUL OF STRINGS: which file to
// edit, where to copy, what to put on a command line. Those strings live here, in the registry, beside the
// launch args and the settings mappings (#103) that already work this way.
//
// This header is the SCHEMA and the PURE decisions over it: parse/serialize, placeholder expansion, title-id
// derivation, and the two restraint verdicts (would this write clobber something the user owns?). It is
// header-only and QtCore-only DELIBERATELY, because EmulatorRegistry.h is header-only and carries a
// ContentSpec on every ExternalEmulator — a probe that only wants the registry must not have to link a
// hashing/-IO translation unit. The machinery that touches disk (sidecar discovery, the install record, the
// appliers) lives in ContentInstall.h/.cpp, which links HashVerify (#97) for the recorded hashes.
//
// THE FOUR KINDS, and the emulator each of them exists for:
//   * "cli"             — hand the file to the emulator's own installer on the command line.
//                         RPCS3: "--headless --installpkg {file}". DESCRIBED ONLY in increment 1 — the PS3
//                         path in EmulatorManager keeps its existing code (probe_contentinstall asserts the
//                         recipe and that code still agree, so the description cannot rot).
//   * "jsonRegistry"    — the emulator keeps a per-title JSON index of the update / DLC files it should use;
//                         installing is EDITING THAT FILE. Ryujinx/Ryubing: games/<titleId>/updates.json and
//                         dlc.json. WIRED in increment 1.
//   * "copyTree"        — the content is copied into the emulator's own content store at a computed path.
//                         Cemu: mlc01/usr/title/<high>/<low>. WIRED in increment 1.
//   * "emulatorUpdater" — the emulator owns this conversation and we must not reimplement it (#189's own
//                         instruction). Recorded so the surface can SAY so instead of silently doing nothing.
// Anything else is an UNKNOWN kind: it parses, it round-trips, isValid() is false, and the launch path logs
// exactly one line and ignores it. It is never an error and never a crash — a registry file from a newer
// build must degrade, not break the launch of a game that needs none of it.
//
// PLACEHOLDERS use the registry's existing {token} spelling. {file} {name} {titleId} {titleIdHigh}
// {titleIdLow} {data} {emuDir} {appData}. A "cli" recipe's args are cut by the SAME shell-style tokeniser the
// launch args use (#237's rule), and {file} is substituted AFTER the cut, so a path with spaces needs no
// quoting — see cliArgv().
#pragma once
#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace ContentRecipe
{
    // ---- the kinds, in ONE place -------------------------------------------------------------------------
    // Every test for "do we know this kind" goes through knownKinds(); nothing else spells them out, so a
    // fifth kind is added here and the "unknown -> ignored with a log" path keeps working unchanged.
    inline const QStringList& knownKinds()
    {
        static const QStringList k{ QStringLiteral("cli"), QStringLiteral("jsonRegistry"),
                                    QStringLiteral("copyTree"), QStringLiteral("emulatorUpdater") };
        return k;
    }

    // ---- one recipe --------------------------------------------------------------------------------------
    struct Recipe
    {
        QString kind;        // "" = no recipe for this slot; else one of knownKinds(), or an unknown spelling
        QString args;        // cli: the argument template, e.g. "--headless --installpkg {file}"
        QString path;        // jsonRegistry: the per-title index file, e.g. "{data}/games/{titleId}/updates.json"
        QString container;   // jsonRegistry: "object" (default) or "array" — the shape of that file's ROOT
        QJsonObject entry;   // jsonRegistry: the entry template merged into it (strings expanded)
        QString dest;        // copyTree: the destination directory template
        QString note;        // human-readable, shown where the surface has to explain a kind it cannot act on

        bool isEmpty() const { return kind.isEmpty(); }
        // A recipe we can ACT on: a known kind whose required field is present. An unknown kind, or a known
        // kind missing its one required field, is false — the launch path logs it once and skips it.
        bool isValid() const
        {
            if (kind == QLatin1String("cli"))             return !args.isEmpty();
            if (kind == QLatin1String("jsonRegistry"))    return !path.isEmpty() && !entry.isEmpty();
            if (kind == QLatin1String("copyTree"))        return !dest.isEmpty();
            if (kind == QLatin1String("emulatorUpdater")) return true;
            return false;   // unknown kind, or empty
        }
        // True for a kind this build KNOWS but which increment 1 does not act on by itself. Kept separate from
        // isValid() so the two questions ("do we understand it" / "can we do it here") never collapse.
        bool isDelegated() const { return kind == QLatin1String("emulatorUpdater"); }
        // "cli" is described-only in increment 1: RPCS3's PS3 chain owns that conversation already.
        bool isDescribedOnly() const { return kind == QLatin1String("cli"); }
    };

    inline bool operator==(const Recipe& a, const Recipe& b)
    {
        return a.kind == b.kind && a.args == b.args && a.path == b.path && a.container == b.container
            && a.entry == b.entry && a.dest == b.dest && a.note == b.note;
    }
    inline bool operator!=(const Recipe& a, const Recipe& b) { return !(a == b); }

    // ---- an emulator's whole content-install description --------------------------------------------------
    struct Spec
    {
        // Candidate data-directory templates, most-specific first. The FIRST ONE THAT EXISTS wins; if none
        // exists the FIRST is used (and created on demand). This is the same "portable folder or the per-user
        // one" choice EmulatorManager::emulatorSaveDirs already makes per emulator — stated as data here so a
        // recipe's {data} means one thing.
        QStringList dataDirs;
        Recipe updates;
        Recipe dlc;

        bool isEmpty() const { return dataDirs.isEmpty() && updates.isEmpty() && dlc.isEmpty(); }
        const Recipe& forSlot(const QString& slot) const
        {
            return slot == QLatin1String("dlc") ? dlc : updates;
        }
    };

    inline bool operator==(const Spec& a, const Spec& b)
    {
        return a.dataDirs == b.dataDirs && a.updates == b.updates && a.dlc == b.dlc;
    }
    inline bool operator!=(const Spec& a, const Spec& b) { return !(a == b); }

    // The two slot names, in one place (the sidecar folder names ARE these strings — see ContentInstall.h).
    inline QString slotUpdates() { return QStringLiteral("updates"); }
    inline QString slotDlc()     { return QStringLiteral("dlc"); }

    // ---- pure: JSON <-> Recipe/Spec -----------------------------------------------------------------------
    // Canonical: every field written only when non-empty, so fromJson(toJson(r)) == r with ONE spelling per
    // recipe. An UNKNOWN kind is carried verbatim rather than dropped — dropping it would make a newer
    // registry file round-trip lossily, and the "ignored with one log line" decision belongs at launch, not
    // at parse (a recipe for a slot the game has no files in is never even looked at).
    inline QJsonObject toJson(const Recipe& r)
    {
        QJsonObject o;
        if (r.isEmpty()) return o;
        o.insert(QStringLiteral("kind"), r.kind);
        if (!r.args.isEmpty())      o.insert(QStringLiteral("args"), r.args);
        if (!r.path.isEmpty())      o.insert(QStringLiteral("path"), r.path);
        if (!r.container.isEmpty()) o.insert(QStringLiteral("container"), r.container);
        if (!r.entry.isEmpty())     o.insert(QStringLiteral("entry"), r.entry);
        if (!r.dest.isEmpty())      o.insert(QStringLiteral("dest"), r.dest);
        if (!r.note.isEmpty())      o.insert(QStringLiteral("note"), r.note);
        return o;
    }

    inline Recipe recipeFromJson(const QJsonValue& v)
    {
        Recipe r;
        if (!v.isObject()) return r;                       // a non-object (string, array, number) = no recipe
        const QJsonObject o = v.toObject();
        r.kind      = o.value(QStringLiteral("kind")).toString().trimmed();
        r.args      = o.value(QStringLiteral("args")).toString();
        r.path      = o.value(QStringLiteral("path")).toString().trimmed();
        r.container = o.value(QStringLiteral("container")).toString().trimmed();
        if (o.value(QStringLiteral("entry")).isObject()) r.entry = o.value(QStringLiteral("entry")).toObject();
        r.dest      = o.value(QStringLiteral("dest")).toString().trimmed();
        r.note      = o.value(QStringLiteral("note")).toString();
        return r;
    }

    inline QJsonObject toJson(const Spec& s)
    {
        QJsonObject o;
        if (s.isEmpty()) return o;
        if (!s.dataDirs.isEmpty())
        {
            QJsonArray a;
            for (const QString& d : s.dataDirs) a.push_back(d);
            o.insert(QStringLiteral("dataDirs"), a);
        }
        const QJsonObject u = toJson(s.updates), d = toJson(s.dlc);
        if (!u.isEmpty()) o.insert(QStringLiteral("updates"), u);
        if (!d.isEmpty()) o.insert(QStringLiteral("dlc"), d);
        return o;
    }

    inline Spec specFromJson(const QJsonValue& v)
    {
        Spec s;
        if (!v.isObject()) return s;
        const QJsonObject o = v.toObject();
        for (const QJsonValue& e : o.value(QStringLiteral("dataDirs")).toArray())
            if (e.isString() && !e.toString().trimmed().isEmpty()) s.dataDirs << e.toString().trimmed();
        s.updates = recipeFromJson(o.value(QStringLiteral("updates")));
        s.dlc     = recipeFromJson(o.value(QStringLiteral("dlc")));
        return s;
    }

    // ---- pure: placeholder expansion ---------------------------------------------------------------------
    struct Vars
    {
        QString file;        // absolute path of the file (or folder) being installed
        QString name;        // its base name
        QString titleId;     // the BASE game's title id, upper-cased ("" when it could not be derived)
        QString dataDir;     // the emulator's resolved data directory
        QString emuDir;      // the emulator's install directory (where its binary lives)
        QString appData;     // the per-user application-data root
    };

    // {titleId} split for the emulators that address content by its two halves (Cemu's mlc01 layout). A
    // non-16-character id yields empties, which is what makes a recipe using them skip rather than build a
    // half-formed path.
    inline QString titleIdHigh(const QString& id) { return id.size() == 16 ? id.left(8)  : QString(); }
    inline QString titleIdLow (const QString& id) { return id.size() == 16 ? id.right(8) : QString(); }

    inline QString expand(const QString& tmpl, const Vars& v)
    {
        QString s = tmpl;
        s.replace(QStringLiteral("{file}"),        v.file);
        s.replace(QStringLiteral("{name}"),        v.name);
        s.replace(QStringLiteral("{titleIdHigh}"), titleIdHigh(v.titleId));
        s.replace(QStringLiteral("{titleIdLow}"),  titleIdLow(v.titleId));
        s.replace(QStringLiteral("{titleId}"),     v.titleId);
        s.replace(QStringLiteral("{data}"),        v.dataDir);
        s.replace(QStringLiteral("{emuDir}"),      v.emuDir);
        s.replace(QStringLiteral("{appData}"),     v.appData);
        return s;
    }

    // Every STRING inside a JSON entry template, expanded in place (recursing through nested objects and
    // arrays). Non-string leaves are carried through untouched, so an entry may hold the empty arrays and
    // booleans an emulator's own schema requires.
    inline QJsonValue expandValue(const QJsonValue& v, const Vars& vars)
    {
        if (v.isString()) return expand(v.toString(), vars);
        if (v.isArray())
        {
            QJsonArray out;
            for (const QJsonValue& e : v.toArray()) out.push_back(expandValue(e, vars));
            return out;
        }
        if (v.isObject())
        {
            QJsonObject src = v.toObject(), out;
            for (auto it = src.constBegin(); it != src.constEnd(); ++it) out.insert(it.key(), expandValue(it.value(), vars));
            return out;
        }
        return v;
    }
    inline QJsonObject expandEntry(const QJsonObject& e, const Vars& v) { return expandValue(e, v).toObject(); }

    // A template still carrying an unresolved placeholder ("…/{titleId}/…" with no title id) must never be
    // turned into a real path — that is how a frontend writes into a folder literally called "{titleId}".
    inline bool hasUnresolvedPlaceholder(const QString& s)
    {
        return s.contains(QLatin1Char('{')) && s.contains(QLatin1Char('}'));
    }

    // ---- pure: the argv a "cli" recipe produces (#237's rule) ---------------------------------------------
    // Cut shell-style FIRST, substitute {file} per token AFTER, exactly like LaunchOpts::buildArgs — which is
    // why a package path containing spaces has never needed quoting. Empty tokens are dropped.
    inline QStringList cliArgv(const Recipe& r, const QString& fileNative)
    {
        QStringList out;
        if (r.kind != QLatin1String("cli")) return out;
        for (QString t : QProcess::splitCommand(r.args))
        {
            t.replace(QStringLiteral("{file}"), fileNative);
            if (!t.isEmpty()) out << t;
        }
        return out;
    }

    // ---- pure: title-id derivation ------------------------------------------------------------------------
    // Two shapes cover every console this touches:
    //   * 16 hex digits — Switch (0100000000010000) and Wii U (0005000010101C00), usually in [brackets] in a
    //     No-Intro-style file or folder name.
    //   * four letters + five digits — the Sony serial shape (BCUS98148, PCSE00001) used by PS3 and Vita.
    // Returns upper case, or empty when the name says nothing. NEVER guesses from a partial match.
    inline QString titleIdFromName(const QString& name)
    {
        static const QRegularExpression hex16(QStringLiteral("(?<![0-9A-Fa-f])([0-9A-Fa-f]{16})(?![0-9A-Fa-f])"));
        const QRegularExpressionMatch mh = hex16.match(name);
        if (mh.hasMatch()) return mh.captured(1).toUpper();
        static const QRegularExpression serial(QStringLiteral("(?<![A-Za-z0-9])([A-Za-z]{4}[0-9]{5})(?![A-Za-z0-9])"));
        const QRegularExpressionMatch ms = serial.match(name);
        if (ms.hasMatch()) return ms.captured(1).toUpper();
        return QString();
    }

    // ---- pure: the two RESTRAINT verdicts (#189's "never clobber what the user installed") ----------------
    // These are the whole discipline, expressed as decisions over observable facts so they can be pinned and
    // MUTATED in a probe rather than trusted. Nothing else in this feature decides whether to overwrite.
    enum class Verdict
    {
        Write,          // nothing there, or it is ours to replace
        SkipIdentical,  // byte-identical content already in place — the idempotent case
        LeaveAlone      // somebody else's content sits there; report it and do not touch it
    };

    // A file we are about to lay down at `dst`.
    //   dstExists   — is there already a file there
    //   dstSha1     — its payload sha1 (only read when it exists)
    //   srcSha1     — the sha1 of what we would write
    //   weInstalledDst — does OUR install record claim this exact destination (i.e. we put the current file
    //                    there ourselves, so replacing it with a newer package is an upgrade, not a clobber)
    inline Verdict verdictForFile(bool dstExists, const QString& dstSha1, const QString& srcSha1, bool weInstalledDst)
    {
        if (!dstExists) return Verdict::Write;
        if (!srcSha1.isEmpty() && dstSha1.compare(srcSha1, Qt::CaseInsensitive) == 0) return Verdict::SkipIdentical;
        if (weInstalledDst) return Verdict::Write;
        return Verdict::LeaveAlone;
    }

    // A SCALAR key inside a jsonRegistry index (Ryujinx's "selected"). The user pinning a particular update in
    // the emulator's own UI is exactly this key, so it is only ever rewritten when it is absent/blank or when
    // it still points at a path THIS app put there.
    inline Verdict verdictForScalar(bool present, const QString& existing, const QString& desired,
                                    const QStringList& oursPaths)
    {
        if (!present || existing.trimmed().isEmpty()) return Verdict::Write;
        if (existing == desired) return Verdict::SkipIdentical;
        for (const QString& p : oursPaths)
            if (QDir::cleanPath(p).compare(QDir::cleanPath(existing), Qt::CaseInsensitive) == 0) return Verdict::Write;
        return Verdict::LeaveAlone;
    }

    // ---- pure: the per-game override levers (#51's store carries the two strings) -------------------------
    // "" = the default (install what is there); "none" = install nothing for this game; anything else is a
    // PIN, matched case-insensitively as a substring of the package's file name (so "v65536" or "1.0.3" both
    // work without the app having to understand any vendor's versioning).
    inline QString pinNone() { return QStringLiteral("none"); }
    inline bool pinIsNone(const QString& pin) { return pin.compare(pinNone(), Qt::CaseInsensitive) == 0; }
    inline bool pinAccepts(const QString& pin, const QString& fileName)
    {
        if (pin.trimmed().isEmpty()) return true;
        if (pinIsNone(pin)) return false;
        return fileName.contains(pin.trimmed(), Qt::CaseInsensitive);
    }
    // DLC lever: "" / "on" = install, "off" = do not. One spelling per state, and the default is ON because a
    // game's DLC being absent is indistinguishable from the game being broken, which is the report nobody can act on.
    inline QString dlcOff() { return QStringLiteral("off"); }
    inline bool dlcEnabled(const QString& lever) { return lever.compare(dlcOff(), Qt::CaseInsensitive) != 0; }
}
