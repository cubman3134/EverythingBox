// NATIVE PORTS (issue #233) — playing a static recompilation of one N64 game as its own PC executable.
//
// WHAT A PORT IS. Projects like Zelda64Recomp take a retail N64 ROM, statically recompile it into native
// code, and publish an ordinary GitHub Release. The user supplies their own ROM; the port renders on a modern
// GPU at a modern frame rate. Structurally that is **a standalone emulator that can run exactly one game** —
// which is why a port is an `ExternalEmulator` (EmulatorRegistry.h) and reuses that whole tier: the release
// resolution, the per-OS artifact match, the auto-install into `emulators/<id>/`, the binary find-rules, the
// process launch and monitoring. Not one line of that is duplicated here.
//
// THE ONE NEW CONCEPT is the binding. An emulator binds to SYSTEMS (`ExternalEmulator::systems`, which
// EmulationTarget.h turns into a picker row on every game of that system); a port binds to ONE GAME
// (`ExternalEmulator::port`, a NativePortBinding). Those are different fields read by different code, so a
// port structurally CANNOT become "a selectable N64 emulator" the way ares is — offering Zelda64Recomp on
// Super Mario 64 is not a bug that has to be avoided by convention, it is unreachable.
//
// WHY A SEPARATE REGISTRY FROM EmulatorRegistry::all(). Ports live in their own list, not merged into the
// emulator registry, for two reasons that are both properties rather than preferences:
//   * `all()` is pinned by probe_useremulators as byte-for-byte the built-in table when no user data files
//     exist. Ports are not emulators and must not move that rail;
//   * nothing that enumerates emulators — the picker, the emulator manager, `boundEmulatorsFor` — can then
//     surface a port by accident, whatever it asks for. A port reaches the user through exactly one route:
//     the "Native port" verb on the row of the game it is bound to.
// It still IS an ExternalEmulator, so `GameLauncher::runEmulator(port, …)` installs and launches it with no
// new machinery at all.
//
// DATA, NOT CODE — because the catalog rots. The shipped catalog is `native/ports/n64recomp.json`, embedded
// through `native/resources/ports.qrc` so an app update refreshes it, and `<data>/ports/*.json` is merged
// over it (SystemCatalog's #92 arrangement, using EmulatorRegistry's own merge primitives byte-for-byte). A
// user can therefore correct a renamed repo, a changed asset name or a dead entry without a rebuild, which
// matters: of the fourteen ports the community lists, one already 404s under the name it is published as.
//
// NAMING. These are "native ports" in every user-visible string, and each upstream is credited by its own
// name (`displayName`). The recompilation toolchain's developers asked a third-party launcher to stop using
// their project's name, and this app does not use it either.
//
// PURE except for the two file-reading accessors at the bottom (which are QtCore only). probe_ports drives
// every function here headlessly.
#pragma once
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include "AppPaths.h"
#include "EmulatorRegistry.h"

namespace NativePorts
{
    // The resource the shipped catalog is embedded at (native/resources/ports.qrc). Named once so the app,
    // the probe and the qrc alias cannot drift — probe_ports asserts this resource exists and parses.
    inline QString shippedCatalogResource() { return QStringLiteral(":/ports/n64recomp.json"); }

    // The ROM MODES a catalog entry may declare, and which of them this build actually honours.
    //   "menu"   — the port asks for the ROM in its OWN main menu and converts it itself. Implemented.
    //   "beside" — the ROM must be placed next to the port's executable.        Increment 2.
    //   "cli"    — the port takes the ROM path on its command line.             Increment 2.
    // A catalog entry declaring an unimplemented (or unknown, or missing) mode is still a valid entry and
    // still matches its game; what it must not do is claim EB will place the file. Callers ask this and say
    // so. Deliberately a whitelist: an unrecognised spelling reads as "not supported", never as "menu".
    inline bool romModeSupported(const QString& mode) { return mode == QStringLiteral("menu"); }

    // ---- pure: No-Intro-ish game identity -------------------------------------------------------------
    // The region tokens a dump name may carry, mapped onto ONE spelling each. Only these words are read as
    // regions: "(Rev A)", "(En,Fr,De)" and "(Beta)" are parenthesised groups too, and treating an unknown
    // group as a region would refuse every dump that carries one.
    inline QString canonicalRegion(const QString& token)
    {
        const QString t = token.trimmed().toLower();
        if (t == QStringLiteral("usa") || t == QStringLiteral("us") || t == QStringLiteral("u")
            || t == QStringLiteral("ntsc-u") || t == QStringLiteral("na")) return QStringLiteral("usa");
        if (t == QStringLiteral("europe") || t == QStringLiteral("eur") || t == QStringLiteral("e")
            || t == QStringLiteral("pal")) return QStringLiteral("europe");
        if (t == QStringLiteral("japan") || t == QStringLiteral("jpn") || t == QStringLiteral("jp")
            || t == QStringLiteral("j") || t == QStringLiteral("ntsc-j")) return QStringLiteral("japan");
        if (t == QStringLiteral("world") || t == QStringLiteral("w")) return QStringLiteral("world");
        if (t == QStringLiteral("australia") || t == QStringLiteral("aus")) return QStringLiteral("australia");
        if (t == QStringLiteral("korea") || t == QStringLiteral("kor")) return QStringLiteral("korea");
        return QString();
    }

    // What a dump name says about itself: the title match key, the regions it declares, the revision it
    // declares. Any of them may be empty — a name is allowed to say nothing, and "said nothing" is a
    // different answer from "said something else" everywhere below.
    struct GameIdentity
    {
        QString     key;        // the normalised title (see normalizeTitle)
        QStringList regions;    // canonical region tokens the name declared, in order; empty = it declared none
        QString     revision;   // "rev a" / "v1.1" / "" — lowercase, as written
    };

    // Strip a trailing file extension, and ONLY a real one: at most four alphanumeric characters after the
    // last dot. Without that bound "Dr. Mario 64" loses " Mario 64", and a title is not a filename.
    inline QString stripRomExtension(const QString& name)
    {
        const int dot = name.lastIndexOf(QLatin1Char('.'));
        if (dot <= 0) return name;
        const QString ext = name.mid(dot + 1);
        if (ext.isEmpty() || ext.size() > 4) return name;
        for (const QChar c : ext) if (!c.isLetterOrNumber()) return name;
        return name.left(dot);
    }

    // The title match key. Both sides of every comparison go through this, so it only has to be CONSISTENT,
    // not correct in the abstract — and it has exactly one job: make the several ways the same game is spelt
    // on disk collapse onto one string.
    //
    // Worked example, and the reason each step exists. A No-Intro dump is
    //   "Legend of Zelda, The - Majora's Mask (USA).z64"
    // and the same game, scraped into a library, is
    //   "The Legend of Zelda_ Majora's Mask.7z"
    // (a ':' is not a legal Windows filename character, so it lands as '_'). Those two must be one key:
    //   1. drop the extension                 -> "Legend of Zelda, The - Majora's Mask (USA)"
    //   2. drop bracketed groups              -> "Legend of Zelda, The - Majora's Mask"
    //   3. undo the No-Intro article inversion (", The") and drop a LEADING article
    //                                         -> "Legend of Zelda - Majora's Mask" / "Legend of Zelda_ Majora's Mask"
    //   4. lowercase, every non-alphanumeric to a space, collapse
    //                                         -> "legend of zelda majora s mask"  (both)
    // Step 3 is the one that carries the example, and it is the one a mutation test must kill: without it the
    // library's own copy of Majora's Mask does not match the port bound to it.
    inline QString normalizeTitle(const QString& raw)
    {
        QString s = stripRomExtension(raw.trimmed());
        // Everything from the first bracketed group on is dump metadata, not title.
        const int cut = [&] {
            const int p = s.indexOf(QLatin1Char('('));
            const int b = s.indexOf(QLatin1Char('['));
            if (p < 0) return b;
            if (b < 0) return p;
            return qMin(p, b);
        }();
        if (cut >= 0) s = s.left(cut);
        // ", The" / ", A" / ", An" — the No-Intro inversion, which sits mid-string ahead of the subtitle.
        static const QRegularExpression inverted(QStringLiteral(",\\s*(the|an|a)(\\b|$)"),
                                                 QRegularExpression::CaseInsensitiveOption);
        s.replace(inverted, QString());
        // ...and a plain leading article, which is how the same title is written everywhere else.
        static const QRegularExpression leading(QStringLiteral("^\\s*(the|an|a)\\s+"),
                                                QRegularExpression::CaseInsensitiveOption);
        s.replace(leading, QString());
        QString out;
        out.reserve(s.size());
        bool space = false;
        for (const QChar c : s)
        {
            if (c.isLetterOrNumber()) { out.append(c.toLower()); space = false; }
            else if (!out.isEmpty() && !space) { out.append(QLatin1Char(' ')); space = true; }
        }
        while (out.endsWith(QLatin1Char(' '))) out.chop(1);
        return out;
    }

    // Read a dump name (or a catalog title, or a file path's base name) into a GameIdentity.
    inline GameIdentity identify(const QString& nameOrTitle)
    {
        GameIdentity id;
        const QString base = stripRomExtension(QFileInfo(nameOrTitle).fileName().trimmed());
        id.key = normalizeTitle(base);

        static const QRegularExpression group(QStringLiteral("[\\(\\[]([^\\)\\]]*)[\\)\\]]"));
        auto it = group.globalMatch(base);
        while (it.hasNext())
        {
            const QString inner = it.next().captured(1);
            for (const QString& piece : inner.split(QLatin1Char(','), Qt::SkipEmptyParts))
            {
                const QString t = piece.trimmed();
                const QString r = canonicalRegion(t);
                if (!r.isEmpty()) { if (!id.regions.contains(r)) id.regions << r; continue; }
                // A revision marker: "Rev A", "Rev 1", "v1.1". The FIRST one wins; a name carrying two is
                // malformed and the first is as good an answer as any.
                if (!id.revision.isEmpty()) continue;
                static const QRegularExpression rev(QStringLiteral("^(rev\\s*[0-9a-z]+|v[0-9][0-9.]*)$"),
                                                    QRegularExpression::CaseInsensitiveOption);
                if (rev.match(t).hasMatch())
                {
                    QString v = t.toLower();
                    v.replace(QRegularExpression(QStringLiteral("^rev\\s*")), QStringLiteral("rev "));
                    id.revision = v;
                }
            }
        }
        return id;
    }

    // ---- pure: does this port run this game? ------------------------------------------------------------
    // The WHOLE gate, in one function, so the offer and the launch can never disagree about it.
    //
    //   * the entry must BE a port (a bound title), and the system must be the port's system. A port is
    //     bound to one game on one console; without this, a same-named game on another system would match;
    //   * the title key must equal the port's title key or one of its alias keys. Equality, never a prefix
    //     or a "contains": "Legend of Zelda, The - Ocarina of Time" shares eleven characters of prefix with
    //     the Majora's Mask entry, and a port that boots the wrong game is the failure this whole feature
    //     has to not have;
    //   * REGION is a REFUSAL, not a requirement. A name that declares no region (which is most of a scraped
    //     library — "The Legend of Zelda_ Majora's Mask.7z" declares nothing) is accepted and the port's own
    //     check speaks: Zelda64Recomp refuses a non-US dump itself, by name, in its own UI. A name that
    //     DECLARES a region the port does not accept is refused here, because we can say so before spending
    //     a 30 MB download on it. Same rule for revisions.
    //
    // NOT gated on: `sha1`. Increment 1 never opens the file (see NativePortBinding::sha1).
    inline bool matches(const ExternalEmulator& port, const QString& systemId, const GameIdentity& id)
    {
        if (!port.isNativePort()) return false;
        if (systemId.trimmed().isEmpty()) return false;
        if (port.port.system.compare(systemId.trimmed(), Qt::CaseInsensitive) != 0) return false;
        if (id.key.isEmpty()) return false;

        bool titleHit = (id.key == normalizeTitle(port.port.title));
        if (!titleHit)
            for (const QString& a : port.port.aliases)
                if (id.key == normalizeTitle(a)) { titleHit = true; break; }
        if (!titleHit) return false;

        if (!port.port.regions.isEmpty() && !id.regions.isEmpty())
        {
            bool ok = false;
            for (const QString& r : id.regions) if (port.port.regions.contains(r)) { ok = true; break; }
            if (!ok) return false;
        }
        if (!port.port.revisions.isEmpty() && !id.revision.isEmpty()
            && !port.port.revisions.contains(id.revision)) return false;
        return true;
    }

    // The same question asked from the two strings a library row actually carries. BOTH are tried because
    // each knows something the other does not: the FILE name is the one that carries "(USA)" and the No-Intro
    // spelling, while the TITLE is all a not-yet-downloaded catalog row has. A hit on either is a match, and
    // the file name is tried first so its region/revision markers are the ones that get read.
    inline bool matchesRow(const ExternalEmulator& port, const QString& systemId,
                           const QString& title, const QString& fileNameOrPath)
    {
        if (!fileNameOrPath.trimmed().isEmpty() && matches(port, systemId, identify(fileNameOrPath))) return true;
        return !title.trimmed().isEmpty() && matches(port, systemId, identify(title));
    }

    // ---- pure: the catalog --------------------------------------------------------------------------
    // Parse one catalog file's bytes into port entries, over `base`. Reuses EmulatorRegistry's merge
    // primitives exactly: a malformed file is reported and skipped without dropping the base, an entry whose
    // id already exists overrides only the fields it names, a new id is appended.
    inline QList<ExternalEmulator> applyCatalogBytes(QList<ExternalEmulator> base, const QByteArray& bytes,
                                                     const std::function<void(const QString&)>& warn = {})
    {
        QJsonArray entries;
        QString err;
        if (!EmulatorRegistry::parseEntries(bytes, &entries, &err))
        {
            if (warn) warn(err);
            return base;
        }
        return EmulatorRegistry::applyEntries(std::move(base), entries, warn);
    }

    // The shipped catalog, read out of the qrc. Empty (with a warning) if the resource is missing — which is
    // the .qrc-not-wired-up failure, and is why probe_ports asserts on this exact function rather than on the
    // file in the source tree: a .qrc added through target_sources after finalization is silently not
    // embedded, and every other symptom of that is "the verb never appears".
    inline QList<ExternalEmulator> shippedPorts(const std::function<void(const QString&)>& warn = {})
    {
        QFile f(shippedCatalogResource());
        if (!f.open(QIODevice::ReadOnly))
        {
            if (warn) warn(QStringLiteral("the shipped port catalog is not embedded in this build"));
            return {};
        }
        const QByteArray bytes = f.readAll();
        f.close();
        return applyCatalogBytes({}, bytes, warn);
    }

    // <data>/ports — the user's own catalog files, merged over the shipped one exactly as <data>/systems and
    // <data>/emulators are over theirs.
    inline QString dataPortsDir() { return AppPaths::dataDir() + QStringLiteral("/ports"); }

    // The merged catalog. Computed once on first use (a new data file needs a restart, like every other
    // catalog here).
    inline const QList<ExternalEmulator>& all()
    {
        static const QList<ExternalEmulator> merged = EmulatorRegistry::loadDataDir(
            dataPortsDir(),
            shippedPorts([](const QString& m) { qWarning("NativePorts: %s", qUtf8Printable(m)); }),
            [](const QString& m) { qWarning("NativePorts: %s", qUtf8Printable(m)); });
        return merged;
    }

    inline const ExternalEmulator* byId(const QString& id)
    {
        for (const ExternalEmulator& e : all())
            if (e.id == id) return &e;
        return nullptr;
    }

    // THE lookup the UI asks: which port, if any, runs this row's game. First match in catalog order wins;
    // nullptr means "this row has no native port", which is the answer for every row but one.
    inline const ExternalEmulator* portForGame(const QString& systemId, const QString& title,
                                               const QString& fileNameOrPath)
    {
        for (const ExternalEmulator& e : all())
            if (matchesRow(e, systemId, title, fileNameOrPath)) return &e;
        return nullptr;
    }
}
