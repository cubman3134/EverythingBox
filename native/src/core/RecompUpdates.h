// UPDATING A RECOMP THAT WAS BUILT ON THIS MACHINE (issue #248, increment d).
//
// THE PROBLEM THIS SOLVES. Increment (c) can compile a recomp here, out of the user's own dump, in minutes of
// CPU. What it could not do is tell somebody that the thing they built is out of date, and — much more
// importantly — it could not rebuild one WITHOUT DESTROYING THE BUILD THAT WORKED. Staging copied the new
// binary straight over the old one, so a rebuild that produced something broken took the working program with
// it, and the only way back was another twenty-minute compile against a catalogue that had already moved.
//
// THE THREE RULES, and every function here exists to serve one of them:
//
//   1. A ROW SAYS `update available` WHEN THE CATALOGUE NAMES A NEWER ENGINE OR A CHANGED RECIPE — and it is
//      compared against WHAT THE BUILD ITSELF RECORDED, not against a timestamp and not against the
//      catalogue's own revision. A catalogue is republished whenever any submission in it is approved; a
//      build is not out of date because somebody else's entry changed. So `builtAtMs` is written into the
//      stamp and DELIBERATELY NEVER READ BY THE COMPARISON (it is there for a person reading the file), and
//      the feed's `release_tag` / `catalog_date` are not in the stamp at all.
//
//   2. A REBUILD IS EXPLICIT. Nothing in this header starts one, and nothing anywhere on the refresh path
//      may: the row OFFERS a rebuild and a person presses it. That is not a convention — probe_recompbuild
//      asserts that the feed's own translation units do not so much as name RecompBuildJob, and that the one
//      call that starts a build lives in the card's verb switch.
//
//   3. THE PREVIOUS BUILD IS KEPT UNTIL THE NEW ONE HAS RUN ONCE. This is the whole safety property. A
//      rebuild MOVES the install aside (it does not copy it — a move is atomic and cannot half-succeed) and
//      stages the new one into the empty place. From then until the new program has actually run, both are on
//      disk, the row says so, and `restoreKept` puts the old one back in one rename. Only a launch that
//      PROVED ITSELF (below) removes the old copy.
//
// WHAT COUNTS AS "IT RAN". `launchProvesBuild` — and it is deliberately conservative, because the two
// mistakes are not symmetric: keeping a dead build costs disk, and dropping a live one costs the user their
// working program. A process that never started proves nothing. A process the user closed themselves ran, by
// definition. Otherwise it has to have stayed up for `kLaunchProvesMs`, which is GameLauncher's own existing
// threshold for "closed immediately = a failed boot", reused rather than re-chosen so the app cannot hold two
// opinions about the same four seconds.
//
// WHY THE COMPARISON IS FIELD BY FIELD AND NOT A DIGEST. A hash over the recipe would be one line, and it
// would mean that the first EverythingBox release to add a tenth field to the recipe declared every recomp on
// every machine out of date on upgrade day. So the stamp carries the FIELDS, the comparison walks them, and a
// field that either side leaves empty is UNKNOWN — never a difference. That is the same rule the rest of this
// feature already runs on (RecompRows.h: "an unknown input is never read as a negative").
//
// PURE ABOVE THE LINE. Everything down to `restoreKept`'s declaration is (recorded facts + catalogue facts)
// -> a verdict or a sentence: no file, no clock, no process. The four functions below the line move
// directories and are in RecompUpdates.cpp; they take the install directory as an ARGUMENT rather than
// asking EmulatorManager for it, which is what keeps this unit linkable into a QtCore-only probe.
#pragma once
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace recompupdate
{
    // ---- versions ---------------------------------------------------------------------------------------
    // Engine versions are written by three different projects and this app orders them without pretending
    // they are all semver. A version is a run of dot-separated numbers, optionally preceded by `v`, followed
    // by whatever the publisher put after it.
    struct Version
    {
        QVector<qint64> parts;
        QString         suffix;   // "-rc1", "+meta", "" for a plain release
        bool            ok = false;   // at least one numeric component; "" and "nightly" are not orderable
    };

    inline Version parseVersion(const QString& raw)
    {
        Version v;
        QString s = raw.trimmed().toLower();
        if (s.startsWith(QLatin1Char('v'))) s.remove(0, 1);
        int i = 0;
        while (i < s.size() && s.at(i).isDigit())
        {
            int j = i;
            while (j < s.size() && s.at(j).isDigit()) ++j;
            // Bounded: a "version" of forty digits is not one, and toLongLong would answer 0 for it. Taking
            // the first 18 keeps it a number rather than silently making every long run equal.
            v.parts.push_back(s.mid(i, qMin(j - i, 18)).toLongLong());
            v.ok = true;
            i = j;
            if (i < s.size() && s.at(i) == QLatin1Char('.')) { ++i; continue; }
            break;
        }
        v.suffix = s.mid(i);
        return v;
    }

    inline bool versionsOrderable(const QString& a, const QString& b)
    {
        return parseVersion(a).ok && parseVersion(b).ok;
    }

    // -1 / 0 / 1. A missing component is a zero ("1.4" == "1.4.0"), and a SUFFIX makes a version older than
    // the same numbers without one — "1.4.0-rc1" precedes "1.4.0", which is the one piece of semver
    // precedence that publishers actually rely on.
    inline int compareVersions(const QString& a, const QString& b)
    {
        const Version x = parseVersion(a);
        const Version y = parseVersion(b);
        const int n = qMax(x.parts.size(), y.parts.size());
        for (int i = 0; i < n; ++i)
        {
            const qint64 xa = i < x.parts.size() ? x.parts.at(i) : 0;
            const qint64 yb = i < y.parts.size() ? y.parts.at(i) : 0;
            if (xa != yb) return xa < yb ? -1 : 1;
        }
        const bool xs = !x.suffix.trimmed().isEmpty();
        const bool ys = !y.suffix.trimmed().isEmpty();
        if (xs != ys) return xs ? -1 : 1;
        const int c = QString::compare(x.suffix, y.suffix);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }

    // ---- the recipe -------------------------------------------------------------------------------------
    // The catalogue fields that decide WHAT GETS BUILT. Everything else an entry carries — its name, its
    // description, its licence string, its author notes, the feed's own release tag — is excluded on purpose:
    // those change without the program changing, and a row that said `update available` because a typo in a
    // description was fixed would teach people to ignore the label.
    struct BuildRecipe
    {
        QString engine;           // build.generate.engine
        QString sourceRepo;       // build.source.github
        QString sourceRef;        // build.source.ref — the tag/branch/commit the source came from
        QString sdkId;            // build.sdk.id
        QString generateConfig;   // build.generate.config
        QString generateOutDir;   // build.generate.out_dir
        QString cmakeDir;         // build.cmake.build_dir
        QString cmakeTarget;      // build.cmake.target
        QString cmakeConfig;      // build.cmake.config

        bool isEmpty() const
        {
            return engine.trimmed().isEmpty() && sourceRepo.trimmed().isEmpty()
                && sourceRef.trimmed().isEmpty() && sdkId.trimmed().isEmpty()
                && generateConfig.trimmed().isEmpty() && generateOutDir.trimmed().isEmpty()
                && cmakeDir.trimmed().isEmpty() && cmakeTarget.trimmed().isEmpty()
                && cmakeConfig.trimmed().isEmpty();
        }
    };

    // The catalogue's side of the comparison.
    struct CatalogueBuild
    {
        QString     engineVersion;   // build.sdk.version / build.generate.engine_version
        BuildRecipe recipe;
    };

    // ONE FIELD, COMPARED. Empty on either side is UNKNOWN and never a difference — which covers both a
    // catalogue that has stopped pinning something and a stamp written by a build of this app that did not
    // record that field yet.
    inline bool fieldDiffers(const QString& recorded, const QString& current)
    {
        const QString a = recorded.trimmed();
        const QString b = current.trimmed();
        if (a.isEmpty() || b.isEmpty()) return false;
        return a.compare(b, Qt::CaseInsensitive) != 0;
    }

    // WHAT CHANGED, in words, for the card. Returns an empty list when nothing did — so it doubles as the
    // predicate, and the sentence a person reads and the decision the row makes cannot disagree.
    inline QStringList recipeChanges(const BuildRecipe& built, const BuildRecipe& cat)
    {
        struct Field { const char* label; QString a; QString b; };
        const Field fields[] = {
            { "recompiler",       built.engine,         cat.engine },
            { "source",           built.sourceRepo,     cat.sourceRepo },
            { "source version",   built.sourceRef,      cat.sourceRef },
            { "SDK",              built.sdkId,          cat.sdkId },
            { "generator config", built.generateConfig, cat.generateConfig },
            { "generator output", built.generateOutDir, cat.generateOutDir },
            { "build folder",     built.cmakeDir,       cat.cmakeDir },
            { "build target",     built.cmakeTarget,    cat.cmakeTarget },
            { "build type",       built.cmakeConfig,    cat.cmakeConfig },
        };
        QStringList out;
        for (const Field& f : fields)
            if (fieldDiffers(f.a, f.b))
                out << QStringLiteral("%1: %2 → %3")
                           .arg(QString::fromLatin1(f.label), f.a.trimmed(), f.b.trimmed());
        return out;
    }

    inline bool recipeChanged(const BuildRecipe& built, const BuildRecipe& cat)
    {
        return !recipeChanges(built, cat).isEmpty();
    }

    // ---- what a build records about itself --------------------------------------------------------------
    // Written into the install folder the moment a build succeeds, next to the program it describes, so
    // removing the port removes it — the same lifetime, and the same reasoning, as
    // NativePorts::writeInstalledTag for the pre-built tier.
    struct BuildStamp
    {
        QString     portId;
        QString     engineVersion;
        BuildRecipe recipe;
        // INFORMATIONAL ONLY. Written so a person opening the file can see when this was built; read by
        // nothing. Comparing timestamps is how "the catalogue was republished" becomes "your build is old".
        qint64      builtAtMs = 0;
        // Has the program this stamp describes actually run? False from the moment it is staged until a
        // launch proves it, and it is what keeps the previous build alive across a restart.
        bool        launched = false;
        bool        valid = false;   // false = there is no stamp, which is "nobody knows", never "out of date"
    };

    inline constexpr int kStampSchema = 1;

    inline QByteArray encodeStamp(const BuildStamp& s)
    {
        QJsonObject r;
        r[QStringLiteral("engine")] = s.recipe.engine;
        r[QStringLiteral("source_repo")] = s.recipe.sourceRepo;
        r[QStringLiteral("source_ref")] = s.recipe.sourceRef;
        r[QStringLiteral("sdk_id")] = s.recipe.sdkId;
        r[QStringLiteral("generate_config")] = s.recipe.generateConfig;
        r[QStringLiteral("generate_out_dir")] = s.recipe.generateOutDir;
        r[QStringLiteral("cmake_dir")] = s.recipe.cmakeDir;
        r[QStringLiteral("cmake_target")] = s.recipe.cmakeTarget;
        r[QStringLiteral("cmake_config")] = s.recipe.cmakeConfig;

        QJsonObject o;
        o[QStringLiteral("schema")] = kStampSchema;
        o[QStringLiteral("port_id")] = s.portId;
        o[QStringLiteral("engine_version")] = s.engineVersion;
        o[QStringLiteral("recipe")] = r;
        o[QStringLiteral("built_at_ms")] = double(s.builtAtMs);
        o[QStringLiteral("launched")] = s.launched;
        return QJsonDocument(o).toJson(QJsonDocument::Indented);
    }

    inline BuildStamp decodeStamp(const QByteArray& bytes)
    {
        BuildStamp s;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) return s;
        const QJsonObject o = doc.object();
        // A schema from the future is not read. Guessing at a document a later build wrote is how a stamp
        // comes to be interpreted as saying something it does not say — and the honest answer, `valid ==
        // false`, is already "nobody knows which build this is", which never claims an update.
        if (o.value(QStringLiteral("schema")).toInt(0) > kStampSchema) return s;
        s.portId = o.value(QStringLiteral("port_id")).toString();
        s.engineVersion = o.value(QStringLiteral("engine_version")).toString();
        const QJsonObject r = o.value(QStringLiteral("recipe")).toObject();
        s.recipe.engine = r.value(QStringLiteral("engine")).toString();
        s.recipe.sourceRepo = r.value(QStringLiteral("source_repo")).toString();
        s.recipe.sourceRef = r.value(QStringLiteral("source_ref")).toString();
        s.recipe.sdkId = r.value(QStringLiteral("sdk_id")).toString();
        s.recipe.generateConfig = r.value(QStringLiteral("generate_config")).toString();
        s.recipe.generateOutDir = r.value(QStringLiteral("generate_out_dir")).toString();
        s.recipe.cmakeDir = r.value(QStringLiteral("cmake_dir")).toString();
        s.recipe.cmakeTarget = r.value(QStringLiteral("cmake_target")).toString();
        s.recipe.cmakeConfig = r.value(QStringLiteral("cmake_config")).toString();
        s.builtAtMs = qint64(o.value(QStringLiteral("built_at_ms")).toDouble(0));
        s.launched = o.value(QStringLiteral("launched")).toBool(false);
        s.valid = true;
        return s;
    }

    // ---- the comparison ---------------------------------------------------------------------------------
    enum class Update
    {
        Unknown,          // no stamp, or nothing to compare — never presented as being out of date
        UpToDate,         // ...including the case the catalogue was republished with this entry unchanged
        EngineNewer,      // the catalogue names a HIGHER engine version than the one this was built with
        EngineChanged,    // ...or a different one that cannot be ordered against it (a date, a codename)
        RecipeChanged,    // same engine, but what gets built has changed
        CatalogueBehind,  // the catalogue went BACKWARDS. Not an update, and not silence either.
    };

    inline bool updateAvailable(Update u)
    {
        return u == Update::EngineNewer || u == Update::EngineChanged || u == Update::RecipeChanged;
    }

    // THE COMPARISON. Read in order; each clause is the reason the next one is reachable.
    inline Update compareBuild(const BuildStamp& built, const CatalogueBuild& cat)
    {
        // No record of what this install is. RecompRows.h's rule, applied here: an unknown is not a negative,
        // and telling somebody their software is out of date on the strength of a fact nobody has is worse
        // than saying nothing.
        if (!built.valid) return Update::Unknown;

        const QString a = built.engineVersion.trimmed();
        const QString b = cat.engineVersion.trimmed();
        if (!a.isEmpty() && !b.isEmpty())
        {
            if (versionsOrderable(a, b))
            {
                const int c = compareVersions(a, b);
                // A CATALOGUE THAT WENT BACKWARDS. It happens — a release is yanked and the pin reverts —
                // and the one thing it must never do is present as an update, because pressing that button
                // spends twenty minutes of somebody's processor going downhill.
                if (c > 0) return Update::CatalogueBehind;
                if (c < 0) return Update::EngineNewer;
            }
            else if (a.compare(b, Qt::CaseInsensitive) != 0)
            {
                // Two versions that are not numbers (a date, a codename, a commit). Different is all that can
                // honestly be said, and it is said as `changed` rather than as `newer`.
                return Update::EngineChanged;
            }
        }
        // Same engine, or an engine version nobody published. What is left is the recipe.
        return recipeChanged(built.recipe, cat.recipe) ? Update::RecipeChanged : Update::UpToDate;
    }

    // ---- the sentences ----------------------------------------------------------------------------------
    // Beside the comparison, for the reason the rest of this feature keeps its wording beside its decisions:
    // a probe and the screen must not be able to disagree about what somebody was told.
    inline QString updateSentence(Update u, const BuildStamp& built, const CatalogueBuild& cat)
    {
        switch (u)
        {
            case Update::Unknown:
                return QStringLiteral("EverythingBox doesn't have a record of how this copy was built, so it "
                                      "can't tell you whether it is out of date.");
            case Update::UpToDate:
                return QStringLiteral("This is built from what the catalogue lists today.");
            case Update::EngineNewer:
                return QStringLiteral("The catalogue now lists %1 %2; this copy was built with %3. Rebuilding "
                                      "is up to you — it compiles the game again on this computer and takes "
                                      "as long as the first time did.")
                    .arg(built.recipe.engine.isEmpty() ? QStringLiteral("the recompiler")
                                                       : built.recipe.engine,
                         cat.engineVersion.trimmed(), built.engineVersion.trimmed());
            case Update::EngineChanged:
                return QStringLiteral("The catalogue now lists a different build of %1 (%2 rather than %3). "
                                      "Rebuilding is up to you.")
                    .arg(built.recipe.engine.isEmpty() ? QStringLiteral("the recompiler")
                                                       : built.recipe.engine,
                         cat.engineVersion.trimmed(), built.engineVersion.trimmed());
            case Update::RecipeChanged:
            {
                const QStringList what = recipeChanges(built.recipe, cat.recipe);
                return QStringLiteral("The catalogue has changed how this one is built (%1). Rebuilding is up "
                                      "to you.")
                    .arg(what.join(QStringLiteral("; ")));
            }
            case Update::CatalogueBehind:
                // Said out loud rather than hidden. Somebody who watched the row say `update available`
                // yesterday deserves to know why it stopped, and "the catalogue went back" is the answer.
                return QStringLiteral("The catalogue now lists an OLDER version (%1) than the one this was "
                                      "built with (%2), so there is nothing to update to.")
                    .arg(cat.engineVersion.trimmed(), built.engineVersion.trimmed());
        }
        return QString();
    }

    // ---- what proves a build ----------------------------------------------------------------------------
    // GameLauncher's own number for "that was a failed boot, not a play session". Reused, not re-chosen.
    inline constexpr qint64 kLaunchProvesMs = 4000;

    // Did that run prove the new build works? Conservative on purpose: the two mistakes are not symmetric.
    // Keeping a dead copy costs disk; dropping a live one costs somebody the program that worked.
    inline bool launchProvesBuild(bool started, qint64 upMs, bool userClosed)
    {
        if (!started) return false;       // it never became a process. Nothing was proved.
        if (userClosed) return true;      // the user closed it themselves, so it was up and they were in it.
        return upMs >= kLaunchProvesMs;   // otherwise it has to have stayed up.
    }

    // ---- the disk cost, said out loud -------------------------------------------------------------------
    // #248 (d) item 5: two builds of one title exist between a rebuild and its first run, and that is not
    // allowed to be a surprise. `bytes` < 0 means the size could not be measured, and then the sentence says
    // what is true without a number rather than saying "0 MB".
    inline QString megabytes(qint64 bytes)
    {
        if (bytes < 0) return QString();
        const double mb = double(bytes) / (1024.0 * 1024.0);
        return mb >= 10.0 ? QStringLiteral("%1 MB").arg(qRound(mb))
                          : QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    }

    inline QString keptCopySentence(const QString& title, qint64 keptBytes, const QString& keptPath)
    {
        const QString size = megabytes(keptBytes);
        const QString where = keptPath.trimmed().isEmpty() ? QStringLiteral("this computer") : keptPath.trimmed();
        return size.isEmpty()
                   ? QStringLiteral("The build of %1 that worked before this one is still on this computer, "
                                    "at %2. It is removed the first time the new one runs — until then both "
                                    "are taking up space.")
                         .arg(title, where)
                   : QStringLiteral("The build of %1 that worked before this one is still on this computer — "
                                    "%2 at %3. It is removed the first time the new one runs — until then "
                                    "both are taking up space.")
                         .arg(title, size, where);
    }

    inline QString keptRemovedSentence(const QString& title, qint64 freedBytes)
    {
        const QString size = megabytes(freedBytes);
        return size.isEmpty()
                   ? QStringLiteral("%1 ran, so the previous build has been removed.").arg(title)
                   : QStringLiteral("%1 ran, so the previous build has been removed and %2 given back.")
                         .arg(title, size);
    }

    inline QString keptSurvivedSentence(const QString& title)
    {
        return QStringLiteral("%1 closed straight away, so the build that worked before it has been kept. "
                              "Open its row and choose “Go back a build” to put it "
                              "back.")
            .arg(title);
    }

    inline QString restoredSentence(const QString& title)
    {
        return QStringLiteral("Put %1 back to the build that worked before. The one that replaced it has been "
                              "removed; nothing else was changed.")
            .arg(title);
    }

    // The refusal when a rebuild cannot get the old build out of the way. It is a REFUSAL and not a warning:
    // staging over an install we could not move aside is exactly the thing this increment exists to stop.
    inline QString cannotKeepSentence(const QString& title)
    {
        return QStringLiteral("%1 wasn't rebuilt: the copy already installed couldn't be moved out of the "
                              "way, and this app will not build over one it cannot put back. It may still be "
                              "running — close it and try again.")
            .arg(title);
    }

    // ---- the files (RecompUpdates.cpp) ------------------------------------------------------------------
    // Every one of these takes the port's INSTALL DIRECTORY. Not a port, not an id: this unit must stay
    // linkable into a QtCore-only probe, and EmulatorManager (which is the one thing that knows where an
    // install lives) drags a network stack behind it.

    // <installDir>/eb-recomp-build.json
    QString stampPath(const QString& installDir);
    BuildStamp readStamp(const QString& installDir);
    bool       writeStamp(const QString& installDir, const BuildStamp& s);
    // Flip `launched` to true, in place, keeping everything else. Returns false when there is no stamp.
    bool       markLaunched(const QString& installDir);

    // <installDir>/../.eb-previous/<name> — the kept copy. A SIBLING of the install rather than somewhere
    // under <data>, and that is load-bearing: keeping it beside the install guarantees the same volume, and a
    // same-volume rename is atomic. A copy could half-succeed, and half a previous build is worse than none.
    // The dot prefix keeps it out of anything that lists the emulators folder.
    QString keptDirFor(const QString& installDir);
    bool    hasKept(const QString& installDir);
    qint64  keptBytes(const QString& installDir);   // -1 when there is none
    qint64  dirBytes(const QString& dir);           // -1 when it does not exist

    // Move the installed build out of the way, dropping any older kept copy FIRST — so at most one kept copy
    // per port exists no matter how many times somebody rebuilds. False (with *why) when there was something
    // there and it could not be moved; TRUE with no kept copy when there was nothing there to keep, which is
    // the first-build case.
    bool keepAside(const QString& installDir, QString* why = nullptr);
    bool dropKept(const QString& installDir);
    // Put the kept copy back, removing whatever replaced it. The stamp travels with the folder, so the row
    // goes straight back to saying `update available` against the same catalogue.
    bool restoreKept(const QString& installDir, QString* why = nullptr);
}
