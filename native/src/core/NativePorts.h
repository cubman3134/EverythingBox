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
// THE CATALOG SCHEMA IS SOMEBODY ELSE'S, ON PURPOSE. `native/ports/*.json` uses the per-title field names of
// the RetComM catalog (github.com/TechnicallyComputers/retcomm-catalog, SCHEMA.md): `id`, `name`, `kind`,
// `platform`, `release.github` + `release.asset_glob.*`, `rom_identity.{crc32,md5,sha1,sha256,sizes,filenames,
// disc_serials,require_cue,track_counts}`, `rom_extensions`, `install_dir_name`, `launch.{windows,linux,macos}`,
// `author_notes`, `notes`. That schema already encodes exactly the game identity this feature needs, in
// digest shapes HashVerify can consume — so a later increment can read that feed unchanged instead of
// translating it. Two consequences worth stating rather than discovering:
//   * in that schema `name` is the GAME, and the port's own project is credited by the OWNER segment of
//     `release.github` (that is how RetComM's own hub labels the author). EB does the same, so
//     ExternalEmulator::displayName for our one entry is "Zelda64Recomp";
//   * `build` is deliberately absent from our entries. Its `generate.engine` enum is snesrecomp|psxrecomp|
//     gbarecomp; N64 has no generic recompiler (N64Recomp needs a per-game ELF+TOML out of a decomp), so N64
//     titles are download-only, which is what this increment implements.
// ONE FIELD IS OURS AND IS MARKED AS OURS: `rom_delivery` (NativePortBinding::romDelivery), because that
// schema does not say how a port takes the ROM and EB has to know before it can help.
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
// DATA, NOT CODE — because the catalog rots. The shipped catalog is embedded through
// `native/resources/ports.qrc` so an app update refreshes it, and `<data>/ports/*.json` is merged over it
// (SystemCatalog's #92 arrangement). A user can therefore correct a renamed repo, a changed asset name or a
// dead entry without a rebuild, which matters: of the fourteen ports the community lists, one already 404s
// under the name it is published as.
//
// NAMING. These are "native ports" in every user-visible string, and each upstream is credited by its own
// name. The recompilation toolchain's developers asked a third-party launcher to stop using their project's
// name, and this app does not use it either.
//
// PURE except for the two file-reading accessors at the bottom (which are QtCore only). probe_ports drives
// every function here headlessly.
#pragma once
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <functional>

#include "AppPaths.h"
#include "EmulatorRegistry.h"

namespace NativePorts
{
    // The resource the shipped catalog is embedded at (native/resources/ports.qrc). Named once so the app,
    // the probe and the qrc alias cannot drift — probe_ports asserts this resource exists and parses.
    inline QString shippedCatalogResource() { return QStringLiteral(":/ports/n64recomp.json"); }

    // The ROM-DELIVERY modes a catalog entry may declare (our extension to the RetComM schema), and which of
    // them this build actually honours.
    //   "in_app_menu" — the port asks for the ROM in its OWN main menu and converts it itself. Implemented.
    //   "beside_exe"  — the ROM must be placed next to the port's executable.        Increment 2.
    //   "cli_path"    — the port takes the ROM path on its command line.             Increment 2.
    // An entry declaring an unimplemented (or unknown, or missing) mode is still a valid entry and still
    // matches its game; what it must not do is claim EB will place the file. Callers ask this and say so.
    // Deliberately a whitelist: an unrecognised spelling reads as "not supported", never as "in_app_menu".
    inline bool romDeliverySupported(const QString& mode) { return mode == QStringLiteral("in_app_menu"); }

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
    // Worked example, and the reason each step exists. The catalog's No-Intro basename is
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

    // ---- pure: what the catalog entry itself accepts ---------------------------------------------------
    // Every spelling of the bound game's title the entry offers: its `name`, plus each `rom_identity.filenames`
    // basename. RetComM calls those filenames "search hints, not hard matching" for its own hub — for us they
    // are also match candidates, because a library stores the dump under exactly one of them.
    inline QStringList titleKeys(const ExternalEmulator& port)
    {
        QStringList keys;
        const QString n = normalizeTitle(port.port.name);
        if (!n.isEmpty()) keys << n;
        for (const QString& f : port.port.filenames)
        {
            const QString k = normalizeTitle(f);
            if (!k.isEmpty() && !keys.contains(k)) keys << k;
        }
        return keys;
    }

    // The regions this entry accepts, derived from its declared `rom_identity.filenames` — because the RetComM
    // schema has NO region field, and a No-Intro basename is the only place the region is written down.
    // Empty (no filename declared a region) means "accepts any", which is the honest reading of an entry that
    // never said.
    inline QStringList acceptedRegions(const ExternalEmulator& port)
    {
        QStringList out;
        for (const QString& f : port.port.filenames)
            for (const QString& r : identify(f).regions)
                if (!out.contains(r)) out << r;
        return out;
    }

    // ...and the same for revisions.
    inline QStringList acceptedRevisions(const ExternalEmulator& port)
    {
        QStringList out;
        for (const QString& f : port.port.filenames)
        {
            const QString r = identify(f).revision;
            if (!r.isEmpty() && !out.contains(r)) out << r;
        }
        return out;
    }

    // ---- pure: does this port run this game? ------------------------------------------------------------
    // The WHOLE gate, in one function, so the offer and the launch can never disagree about it.
    //
    //   * the entry must BE a port (a bound game name), and the system must be the port's `platform`. A port
    //     is bound to one game on one console; without this, a same-named game on another system would match;
    //   * the title key must equal one of the entry's title keys. Equality, never a prefix or a "contains":
    //     "Legend of Zelda, The - Ocarina of Time" shares eleven characters of prefix with the Majora's Mask
    //     entry, and a port that boots the wrong game is the failure this whole feature has to not have;
    //   * REGION is a REFUSAL, not a requirement. A name that declares no region (which is most of a scraped
    //     library — "The Legend of Zelda_ Majora's Mask.7z" declares nothing) is accepted and the port's own
    //     check speaks: Zelda64Recomp refuses a non-US dump itself, by name, in its own UI. A name that
    //     DECLARES a region the entry does not accept is refused here, because we can say so before spending
    //     a 30 MB download on it. Same rule for revisions.
    //
    // NOT gated on: the `rom_identity` digests. Increment 1 never opens the file (see NativePortBinding).
    inline bool matches(const ExternalEmulator& port, const QString& systemId, const GameIdentity& id)
    {
        if (!port.isNativePort()) return false;
        if (systemId.trimmed().isEmpty()) return false;
        if (port.port.platform.compare(systemId.trimmed(), Qt::CaseInsensitive) != 0) return false;
        if (id.key.isEmpty()) return false;
        if (!titleKeys(port).contains(id.key)) return false;

        const QStringList regions = acceptedRegions(port);
        if (!regions.isEmpty() && !id.regions.isEmpty())
        {
            bool ok = false;
            for (const QString& r : id.regions) if (regions.contains(r)) { ok = true; break; }
            if (!ok) return false;
        }
        const QStringList revisions = acceptedRevisions(port);
        if (!revisions.isEmpty() && !id.revision.isEmpty() && !revisions.contains(id.revision)) return false;
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

    // ---- pure: RetComM shapes -> the shapes EB's standalone tier already speaks ------------------------
    // An asset GLOB ("*-Windows.zip", "*win64*", "bpe-*linux*") reduced to the substring EmulatorManager's
    // assetMatches actually tests — it does a `contains` plus an archive-extension check, not a glob match.
    // Leading/trailing '*' are the common case and vanish; an internal '*' leaves several literal segments and
    // the LONGEST wins, because it is the most selective thing the glob asserts. A glob that is only wildcards
    // yields "", which platformArtifact() already treats as "no build for this OS".
    inline QString globToMarker(const QString& glob)
    {
        QString best;
        for (const QString& seg : glob.split(QLatin1Char('*'), Qt::SkipEmptyParts))
        {
            const QString s = seg.trimmed();
            if (s.size() > best.size()) best = s;
        }
        return best;
    }

    // The port's credited name: the OWNER segment of `release.github`. That is RetComM's own rule for
    // labelling the recomp author in its hub, and it is why the catalog's `name` can stay the GAME's name.
    inline QString creditedName(const QString& releaseRepo)
    {
        const int slash = releaseRepo.indexOf(QLatin1Char('/'));
        return slash > 0 ? releaseRepo.left(slash) : releaseRepo;
    }

    // The GitHub releases endpoint a `release.github` names.
    inline QString releaseApiUrl(const QString& releaseRepo)
    {
        if (releaseRepo.isEmpty()) return QString();
        return QStringLiteral("https://api.github.com/repos/") + releaseRepo + QStringLiteral("/releases/latest");
    }

    // ---- pure: one catalog title <-> one ExternalEmulator ----------------------------------------------
    // Field-level over `base`, exactly as EmulatorRegistry::overlay is over an emulator: a key that is absent
    // leaves the base value untouched, so a <data>/ports/*.json file can correct ONE field (a renamed repo, a
    // changed asset name) without restating the title. Nested objects (`release`, `rom_identity`, `launch`)
    // are overlaid field-by-field too, for the same reason.
    inline ExternalEmulator overlayTitle(const ExternalEmulator& base, const QJsonObject& o)
    {
        ExternalEmulator e = base;
        auto str = [&](const QJsonObject& src, const char* key, QString& dst) {
            if (src.contains(QLatin1String(key))) dst = src.value(QLatin1String(key)).toString().trimmed();
        };
        auto arr = [&](const QJsonObject& src, const char* key, QStringList& dst, bool lower) {
            if (src.contains(QLatin1String(key)))
                dst = EmulatorRegistry::jsonStrList(src.value(QLatin1String(key)), lower);
        };

        str(o, "id", e.id);
        str(o, "name", e.port.name);
        str(o, "kind", e.port.kind);
        str(o, "platform", e.port.platform);
        e.port.platform = e.port.platform.toLower();     // system ids are matched case-insensitively
        str(o, "description", e.port.description);
        str(o, "homepage", e.homepage);
        str(o, "author_notes", e.port.authorNotes);
        str(o, "notes", e.port.notes);
        arr(o, "rom_extensions", e.port.romExtensions, true);
        str(o, "install_dir_name", e.port.installDirName);
        str(o, "rom_delivery", e.port.romDelivery);      // OUR extension to the schema
        e.port.romDelivery = e.port.romDelivery.toLower();

        if (o.value(QStringLiteral("release")).isObject())
        {
            const QJsonObject r = o.value(QStringLiteral("release")).toObject();
            str(r, "github", e.port.releaseRepo);
            if (r.contains(QStringLiteral("allow_prerelease")))
                e.port.allowPrerelease = r.value(QStringLiteral("allow_prerelease")).toBool();
            if (r.value(QStringLiteral("asset_glob")).isObject())
            {
                const QJsonObject g = r.value(QStringLiteral("asset_glob")).toObject();
                str(g, "windows", e.port.assetGlobWindows);
                str(g, "linux", e.port.assetGlobLinux);
                str(g, "macos", e.port.assetGlobMacos);
            }
        }
        if (o.value(QStringLiteral("rom_identity")).isObject())
        {
            const QJsonObject r = o.value(QStringLiteral("rom_identity")).toObject();
            arr(r, "crc32", e.port.crc32, true);
            arr(r, "md5", e.port.md5, true);
            arr(r, "sha1", e.port.sha1, true);
            arr(r, "sha256", e.port.sha256, true);
            arr(r, "filenames", e.port.filenames, false);
            arr(r, "disc_serials", e.port.discSerials, false);
            if (r.contains(QStringLiteral("require_cue")))
                e.port.requireCue = r.value(QStringLiteral("require_cue")).toBool();
            if (r.value(QStringLiteral("sizes")).isArray())
            {
                e.port.sizes.clear();
                for (const QJsonValue& v : r.value(QStringLiteral("sizes")).toArray())
                    if (v.isDouble()) e.port.sizes.push_back(qint64(v.toDouble()));
            }
            if (r.value(QStringLiteral("track_counts")).isArray())
            {
                e.port.trackCounts.clear();
                for (const QJsonValue& v : r.value(QStringLiteral("track_counts")).toArray())
                    if (v.isDouble()) e.port.trackCounts.push_back(int(v.toDouble()));
            }
        }
        if (o.value(QStringLiteral("launch")).isObject())
        {
            const QJsonObject l = o.value(QStringLiteral("launch")).toObject();
            str(l, "windows", e.port.launchWindows);
            str(l, "linux", e.port.launchLinux);
            str(l, "macos", e.port.launchMacos);
        }

        // ---- derive the ExternalEmulator half. Everything below is a PROJECTION of the fields above onto
        // the standalone tier's vocabulary, recomputed on every overlay so a corrected `release.github` also
        // corrects the update URL and the credited name rather than leaving a stale pair behind.
        if (!e.port.releaseRepo.isEmpty())
        {
            e.updateJsonUrl = releaseApiUrl(e.port.releaseRepo);
            e.displayName = creditedName(e.port.releaseRepo);
            if (e.homepage.isEmpty())
                e.homepage = QStringLiteral("https://github.com/") + e.port.releaseRepo;
        }
        e.winArtifact   = globToMarker(e.port.assetGlobWindows);
        e.linuxArtifact = globToMarker(e.port.assetGlobLinux);
        e.macArtifact   = globToMarker(e.port.assetGlobMacos);
        // The launch binary, plus the same-named-subfolder fallback every emulator entry here carries (an
        // archive that extracts into a folder of its own name is the commonest packaging accident).
        auto binaries = [](const QString& rel) {
            QStringList out;
            if (rel.isEmpty()) return out;
            out << rel;
            const QString first = rel.section(QLatin1Char('/'), 0, 0);
            const QString stem = QFileInfo(first).completeBaseName();
            if (!stem.isEmpty()) out << stem + QLatin1Char('/') + rel;
            return out;
        };
        e.winBinaries   = binaries(e.port.launchWindows);
        e.linuxBinaries = binaries(e.port.launchLinux);
        e.macBinaries   = binaries(e.port.launchMacos);
        // `rom_extensions` are written WITH a leading dot in that schema; ExternalEmulator::extensions is
        // documented as lowercase and dotless, and this is the one place the two spellings meet.
        e.extensions.clear();
        for (QString x : e.port.romExtensions)
        {
            while (x.startsWith(QLatin1Char('.'))) x.remove(0, 1);
            if (!x.isEmpty()) e.extensions << x;
        }
        // NO `systems`, ever. See the header note: a port that names a system becomes a selectable emulator
        // for every game on it. This is not a default that a catalog file can override — it is not read.
        e.systems.clear();
        // No arguments: a port with rom_delivery "in_app_menu" is launched with none, and the two modes that
        // would want one are increment 2 (which is where an argsTemplate for them belongs).
        e.argsTemplate.clear();
        return e;
    }

    inline ExternalEmulator titleFromJson(const QJsonObject& o) { return overlayTitle(ExternalEmulator{}, o); }

    // ---- pure: merge a set of catalog titles over a base list ------------------------------------------
    // Byte-for-byte EmulatorRegistry::applyEntries' contract, over this schema: a non-object, or one with no
    // `id`, is reported and skipped without dropping the base; a matching id overrides its named fields; a new
    // id is appended; later entries win.
    inline QList<ExternalEmulator> applyTitles(QList<ExternalEmulator> base, const QJsonArray& entries,
                                               const std::function<void(const QString&)>& warn = {})
    {
        auto note = [&](const QString& m) { if (warn) warn(m); };
        int idx = 0;
        for (const QJsonValue& v : entries)
        {
            const int here = idx++;
            if (!v.isObject()) { note(QStringLiteral("entry %1 is not an object — skipped").arg(here)); continue; }
            const QJsonObject o = v.toObject();
            const QString id = o.value(QStringLiteral("id")).toString().trimmed();
            if (id.isEmpty()) { note(QStringLiteral("entry %1 has no \"id\" — skipped").arg(here)); continue; }

            int found = -1;
            for (int i = 0; i < base.size(); ++i) if (base[i].id == id) { found = i; break; }
            if (found >= 0) base[found] = overlayTitle(base[found], o);
            else            base.push_back(titleFromJson(o));
        }
        return base;
    }

    // Parse one catalog file's bytes over `base`. A malformed file is reported and skipped without dropping
    // anything. Accepts either an array of titles or a lone title object (EmulatorRegistry::parseEntries).
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
        return applyTitles(std::move(base), entries, warn);
    }

    // The shipped catalog, read out of the qrc. Empty (with a warning) if the resource is missing — which is
    // the .qrc-not-wired-up failure, and is why probe_ports asserts on this exact function rather than on the
    // file in the source tree: a .qrc that reaches a target the wrong way is silently not embedded, and every
    // other symptom of that is "the verb never appears". (That is not hypothetical here: listing ports.qrc in
    // qt_add_executable's source list produced no rcc output at all — see native/CMakeLists.txt.)
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
    // <data>/emulators are over theirs. Files are read in name order so the merge is deterministic.
    inline QString dataPortsDir() { return AppPaths::dataDir() + QStringLiteral("/ports"); }

    inline QList<ExternalEmulator> loadPortsDir(const QString& dir, const QList<ExternalEmulator>& base,
                                                const std::function<void(const QString&)>& warn = {})
    {
        if (dir.isEmpty()) return base;
        QDir d(dir);
        if (!d.exists()) return base;

        QList<ExternalEmulator> out = base;
        const QFileInfoList files = d.entryInfoList(QStringList{ QStringLiteral("*.json") },
                                                    QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& fi : files)
        {
            QFile f(fi.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly))
            {
                if (warn) warn(QStringLiteral("%1: cannot open — skipped").arg(fi.fileName()));
                continue;
            }
            const QByteArray bytes = f.readAll();
            f.close();
            out = applyCatalogBytes(std::move(out), bytes,
                                    [&](const QString& m) { if (warn) warn(QStringLiteral("%1: %2").arg(fi.fileName(), m)); });
        }
        return out;
    }

    // The merged catalog. Computed once on first use (a new data file needs a restart, like every other
    // catalog here).
    inline const QList<ExternalEmulator>& all()
    {
        static const QList<ExternalEmulator> merged = loadPortsDir(
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
