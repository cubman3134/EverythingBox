// dosbox.conf — the DOS world's shared language, read (issue #191).
//
// #190 made a DOS game LAUNCH with no configuration. This is the tier above it, and it exists because of one
// fact about the DOS scene: every GOG release, every game pack and every "how do I make this run" answer ever
// written is expressed as a `dosbox.conf`. It is how DOS configuration is passed BETWEEN HUMANS. dosbox-pure
// uses libretro core options instead, so a user handed a known-good conf for a title that misbehaves has, as
// of #190, nowhere to put it.
//
// TWO ANSWERS, AND THIS HEADER IS THE FIRST:
//   * TRANSLATE — parse the conf sitting beside the game and map its keys onto the core's options. That is
//     everything below.
//   * HAND OFF — when a STANDALONE DOSBox is the chosen engine, pass the file through with `-conf` and
//     translate nothing. Lossless by construction; it lives in EmulatorRegistry (`confArgs`) and
//     EmulatorManager, not here, because there is no mapping to be done.
//
// A PARTIAL MAPPING IS THE HONEST OUTCOME, AND SAYING SO IS THE FEATURE. A conf has ~90 keys across a dozen
// sections; a libretro core exposes a few dozen options, and they are not the same set. Silently applying six
// of a user's twenty settings and saying "applied" would be worse than doing nothing — the game would still
// misbehave and the conf would look honoured. So `translate()` returns BOTH lists, every ignored key carries a
// REASON, and `report()` builds the sentence that names both. That report is pinned by probe_dosconf.
//
// THE MAPPING IS DATA, NOT C++ (the #190 discipline). Which conf key becomes which core option — and which
// conf VALUES a given core option accepts — lives in native/systems/recipes/msdos.json under each core's
// `conf` block, overridable from <data>/systems/recipes/ with no rebuild. That matters more here than
// anywhere else in #190: dosbox-pure renames and re-values its options between releases, and a hardcoded
// table would turn every such rename into "the conf silently stopped working". The C++ below knows only the
// SHAPE of a mapping, never a key name.
//
// A CONF WE CANNOT READ APPLIES NOTHING. parse() fails with a line number and a reason, and the caller
// reports that instead of seeding a half-read file. Half-applying a conf is the one outcome that is worse
// than ignoring it: the user reads "applied" and debugs the wrong thing.
//
// EVERYTHING HERE IS PURE — bytes and literals in, structs and strings out. No file I/O, no app state, no Qt
// GUI: firmware presence is asked through an injected `exists` predicate exactly as LaunchRecipe.h does it,
// and the conf is chosen from a LIST of file names rather than by scanning a directory. probe_dosconf drives
// every rule below against literals with no filesystem in the way.
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <functional>

namespace DosConf
{
    // ---- the conf, as read ------------------------------------------------------------------------------

    // One `key=value` inside one `[section]`. `line` is 1-based and is carried so a report can point at the
    // file rather than at the app.
    struct Entry
    {
        QString section;   // lowercased ("cpu")
        QString key;       // lowercased ("cycles")
        QString value;     // verbatim, trimmed ("fixed 5000") — NOT lowercased: a value may be a file name
        int     line = 0;
    };

    // A parsed conf. `ok` false => `error` says why, and NOTHING in here may be applied.
    struct File
    {
        QList<Entry> entries;
        QStringList  autoexec;    // the raw [autoexec] lines, in order (never key=value)
        bool         ok = false;
        QString      error;       // "line 7: expected key=value, got \"cycles\""
    };

    // "cpu.cycles" for an entry — the spelling the mapping table and every message use, so there is exactly
    // one way to name a conf setting anywhere in this feature.
    inline QString qualified(const Entry& e) { return e.section + QLatin1Char('.') + e.key; }

    // ---- pure: parse ------------------------------------------------------------------------------------
    // dosbox.conf is INI: `[section]` headers, `key=value` lines, `#` and `%` comments, blank lines ignored.
    // `[autoexec]` is the exception the format itself makes — its body is DOS commands, not settings — so its
    // lines are kept verbatim instead of being forced through the key=value rule.
    //
    // WHAT IS AN ERROR, and why these four. Each is a shape that cannot be read as a setting at all, so
    // accepting it would mean guessing:
    //   * an unclosed `[section` header — everything after it belongs to a section we cannot name;
    //   * an empty `[]` header — same;
    //   * a `key=value` before any section — the same key means different things in different sections
    //     (`cpu.core` and `sblaster.core` are unrelated), so an unsectioned key is unmappable;
    //   * a non-blank, non-comment line inside a settings section with no `=` in it.
    // Anything else — an unknown section, an unknown key, a value we have no mapping for — is NOT an error.
    // It is an ignored entry, which is the honest outcome this feature is built around.
    inline bool parse(const QByteArray& bytes, File* out)
    {
        File f;
        const QString text = QString::fromUtf8(bytes);
        // Split on \n and drop a trailing \r, so a CRLF conf (which is what Windows and GOG ship) reads
        // identically to a LF one. Splitting on QRegularExpression would pull in a dependency this header
        // does not need.
        const QStringList lines = text.split(QLatin1Char('\n'));
        QString section;
        bool inAutoexec = false;
        for (int i = 0; i < lines.size(); ++i)
        {
            QString raw = lines.at(i);
            if (raw.endsWith(QLatin1Char('\r'))) raw.chop(1);
            const int lineNo = i + 1;
            const QString t = raw.trimmed();
            if (t.isEmpty()) continue;
            if (t.startsWith(QLatin1Char('#')) || t.startsWith(QLatin1Char('%'))) continue;

            if (t.startsWith(QLatin1Char('[')))
            {
                if (!t.endsWith(QLatin1Char(']')))
                {
                    f.error = QStringLiteral("line %1: section header %2 is not closed").arg(lineNo).arg(t);
                    if (out) *out = f;
                    return false;
                }
                const QString name = t.mid(1, t.size() - 2).trimmed().toLower();
                if (name.isEmpty())
                {
                    f.error = QStringLiteral("line %1: section header has no name").arg(lineNo);
                    if (out) *out = f;
                    return false;
                }
                section = name;
                inAutoexec = (name == QLatin1String("autoexec"));
                continue;
            }

            if (inAutoexec) { f.autoexec.push_back(t); continue; }

            const int eq = t.indexOf(QLatin1Char('='));
            if (eq < 0)
            {
                f.error = QStringLiteral("line %1: expected key=value, got %2").arg(lineNo).arg(t);
                if (out) *out = f;
                return false;
            }
            if (section.isEmpty())
            {
                f.error = QStringLiteral("line %1: %2 is outside any [section]").arg(lineNo).arg(t);
                if (out) *out = f;
                return false;
            }
            Entry e;
            e.section = section;
            e.key     = t.left(eq).trimmed().toLower();
            e.value   = t.mid(eq + 1).trimmed();
            e.line    = lineNo;
            if (e.key.isEmpty())
            {
                f.error = QStringLiteral("line %1: %2 has no key before the '='").arg(lineNo).arg(t);
                if (out) *out = f;
                return false;
            }
            f.entries.push_back(e);
        }
        f.ok = true;
        if (out) *out = f;
        return true;
    }

    // The value of one setting ("" when absent). Last wins, which is what DOSBox itself does with a key
    // repeated inside a section.
    inline QString value(const File& f, const QString& section, const QString& key)
    {
        QString v;
        for (const Entry& e : f.entries)
            if (e.section == section.toLower() && e.key == key.toLower()) v = e.value;
        return v;
    }

    // ---- the mapping, as data ---------------------------------------------------------------------------

    // One conf key -> one core option. `values` (when non-empty) is BOTH a translation table AND a
    // whitelist: a conf value it does not list is IGNORED with a reason, never passed through. That is
    // deliberate — a libretro core option has a fixed value set, and pushing `machine=svga_et4000` at a core
    // whose option only knows `svga` would be a setting that silently does nothing.
    //
    // `transform` names a value-shape rule for the cases a lookup table cannot express (there are infinitely
    // many cycle counts). Two spellings are honoured:
    //   ""       — pass the conf value through unchanged (with `values` empty) or through the table;
    //   "cycles" — the `cycles=` grammar, see transformCycles below;
    //   "none"   — NEVER translate this key: report it as ignored, carrying this mapping's `note`. That is
    //              not the same as leaving the key out of the table, and the difference is the point: it is
    //              how a recipe says "we know about this setting and here is what to do instead" (the MIDI
    //              device is chosen in Settings, not from the conf) rather than "we have never heard of it".
    // Any OTHER transform ignores the key rather than passing the raw value through, so a data file can never
    // invent a behaviour the code does not have.
    struct Mapping
    {
        QString from;                    // "cpu.cycles" — section.key, lowercase
        QString to;                      // "dosbox_pure_cycles" — the core option key
        QMap<QString, QString> values;   // conf value (lowercased) -> option value; empty = pass through
        QString transform;               // "" | "cycles"
        QString note;                    // optional human phrase used when this key is ignored
    };

    // The `conf` block of one core entry in a launch recipe.
    struct Spec
    {
        QList<Mapping> map;
        // The phrase used for every conf key with no mapping at all. Data, because it names the ENGINE
        // ("DOSBox-Pure has no matching core option") and the recipe knows which engine it is describing.
        QString unmappedNote;
        bool isNull() const { return map.isEmpty(); }
    };

    inline Spec specFromJson(const QJsonValue& v)
    {
        Spec s;
        const QJsonObject o = v.toObject();
        s.unmappedNote = o.value(QStringLiteral("unmappedNote")).toString().trimmed();
        for (const QJsonValue& mv : o.value(QStringLiteral("map")).toArray())
        {
            if (!mv.isObject()) continue;
            const QJsonObject mo = mv.toObject();
            Mapping m;
            m.from      = mo.value(QStringLiteral("from")).toString().trimmed().toLower();
            m.to        = mo.value(QStringLiteral("to")).toString().trimmed();
            m.transform = mo.value(QStringLiteral("transform")).toString().trimmed().toLower();
            m.note      = mo.value(QStringLiteral("note")).toString().trimmed();
            const QJsonObject vals = mo.value(QStringLiteral("values")).toObject();
            for (auto it = vals.constBegin(); it != vals.constEnd(); ++it)
            {
                const QString k = it.key().trimmed().toLower();
                const QString val = it.value().toString().trimmed();
                if (!k.isEmpty() && !val.isEmpty()) m.values.insert(k, val);
            }
            // A mapping that names no conf key or no option key can never fire; dropping it here keeps every
            // consumer free of the check.
            if (m.from.isEmpty() || m.to.isEmpty()) continue;
            s.map.push_back(m);
        }
        return s;
    }

    // ---- pure: translate --------------------------------------------------------------------------------

    struct Applied
    {
        QString from;         // "cpu.cycles"
        QString confValue;    // "fixed 5000"
        QString option;       // "dosbox_pure_cycles"
        QString optionValue;  // "5000"
    };
    struct Ignored
    {
        QString from;         // "sdl.fullscreen", or "autoexec"
        QString confValue;
        QString reason;
    };

    struct Plan
    {
        QMap<QString, QString> options;   // what to seed on the core
        QList<Applied> applied;
        QList<Ignored> ignored;
        bool    ok = false;               // false => the conf could not be read; options is empty
        QString error;
    };

    // `cycles=` in a conf takes four shapes and dosbox-pure's option takes two of them:
    //   "auto"                -> "auto"
    //   "max", "max 80%", "max limit 20000" -> "max"   (the percentage/limit qualifiers have no option form)
    //   "fixed 5000"          -> "5000"
    //   "5000"                -> "5000"
    // Anything else yields "" and the key is ignored with a reason, which is the whole point of returning a
    // string rather than guessing a number.
    inline QString transformCycles(const QString& raw)
    {
        const QString v = raw.trimmed().toLower();
        if (v.isEmpty()) return QString();
        if (v == QLatin1String("auto")) return QStringLiteral("auto");
        const QStringList parts = v.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.isEmpty()) return QString();
        if (parts.first() == QLatin1String("max")) return QStringLiteral("max");
        QString number = parts.first();
        if (number == QLatin1String("fixed"))
        {
            if (parts.size() < 2) return QString();
            number = parts.at(1);
        }
        bool isNum = false;
        const int n = number.toInt(&isNum);
        if (!isNum || n <= 0) return QString();
        return QString::number(n);
    }

    // The whole translation, in one pure call. Every entry of the conf lands in EXACTLY ONE of `applied` or
    // `ignored` — there is no third bucket and nothing is dropped, which is the property probe_dosconf pins
    // and the reason the report can be trusted.
    inline Plan translate(const File& f, const Spec& spec)
    {
        Plan p;
        if (!f.ok) { p.error = f.error; return p; }
        p.ok = true;

        for (const Entry& e : f.entries)
        {
            const QString q = qualified(e);
            const Mapping* hit = nullptr;
            for (const Mapping& m : spec.map) if (m.from == q) { hit = &m; break; }
            if (!hit)
            {
                Ignored ig; ig.from = q; ig.confValue = e.value;
                ig.reason = spec.unmappedNote.isEmpty()
                                ? QStringLiteral("no matching core option") : spec.unmappedNote;
                p.ignored.push_back(ig);
                continue;
            }

            QString outValue;
            if (hit->transform == QLatin1String("none"))
            {
                // Deliberately untranslatable — fall through to the ignored branch carrying the note.
            }
            else if (!hit->values.isEmpty())
            {
                outValue = hit->values.value(e.value.trimmed().toLower());
            }
            else if (hit->transform == QLatin1String("cycles"))
            {
                outValue = transformCycles(e.value);
            }
            else if (hit->transform.isEmpty())
            {
                outValue = e.value.trimmed();
            }
            // else: an unknown transform leaves outValue empty -> ignored, see the Mapping note.

            if (outValue.isEmpty())
            {
                Ignored ig; ig.from = q; ig.confValue = e.value;
                ig.reason = hit->note.isEmpty()
                                ? QStringLiteral("%1 has no setting for this value").arg(hit->to)
                                : hit->note;
                p.ignored.push_back(ig);
                continue;
            }

            Applied ap;
            ap.from = q; ap.confValue = e.value; ap.option = hit->to; ap.optionValue = outValue;
            p.applied.push_back(ap);
            p.options.insert(hit->to, outValue);   // a repeated key: the last one wins, as DOSBox does
        }

        if (!f.autoexec.isEmpty())
        {
            Ignored ig;
            ig.from = QStringLiteral("autoexec");
            ig.confValue = QString::number(f.autoexec.size()) + QStringLiteral(" line(s)");
            ig.reason = QStringLiteral("mounting and start-up commands are the emulator's own job");
            p.ignored.push_back(ig);
        }
        return p;
    }

    // ---- pure: which file beside the game is the conf ----------------------------------------------------
    // `relPaths` is every file under the game folder (relative, any order, any case).
    //
    //   1. `dosbox.conf` at the top of the folder wins outright — the community's own name for it;
    //   2. else a `dosbox*.conf` at the top, choosing the one WITHOUT `_single` in its name: GOG ships a pair,
    //      `dosbox_<game>.conf` (the settings) and `dosbox_<game>_single.conf` (the same settings plus an
    //      autoexec that runs the game and exits), and it is the settings file this feature is about;
    //   3. else, only if there is EXACTLY ONE `.conf` at the top, that one;
    //   4. else nothing. Two unrelated confs is a genuine ambiguity and picking one would be a coin flip that
    //      changed how the game runs.
    // Returns the relative path as it was given (so the caller can open it), or "".
    inline QString chooseConf(const QStringList& relPaths)
    {
        auto isTop = [](const QString& p) {
            QString s = p; s.replace(QLatin1Char('\\'), QLatin1Char('/'));
            return !s.contains(QLatin1Char('/'));
        };
        QStringList tops;
        for (const QString& p : relPaths)
            if (isTop(p) && p.trimmed().toLower().endsWith(QLatin1String(".conf"))) tops.push_back(p.trimmed());
        std::sort(tops.begin(), tops.end(),
                  [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });

        for (const QString& p : tops)
            if (p.compare(QLatin1String("dosbox.conf"), Qt::CaseInsensitive) == 0) return p;

        QStringList dosbox;
        for (const QString& p : tops)
            if (p.toLower().startsWith(QLatin1String("dosbox"))) dosbox.push_back(p);
        if (!dosbox.isEmpty())
        {
            for (const QString& p : dosbox)
                if (!p.toLower().contains(QLatin1String("_single"))) return p;
            return dosbox.first();
        }
        if (tops.size() == 1) return tops.first();
        return QString();
    }

    // ---- pure: THE REPORT -------------------------------------------------------------------------------
    // The sentence the user reads. Names the file, says how many of its settings were applied out of how
    // many, and lists BOTH sides by name. The counts and both lists are the integrity of this feature: a
    // message that said "conf applied" while dropping fourteen keys would pass every test but this one.
    //
    //   “Doom” — DOSBOX.CONF: 3 of 5 settings applied (cpu.cycles → dosbox_pure_cycles=5000,
    //   dosbox.machine → dosbox_pure_machine=svga, dosbox.memsize → dosbox_pure_memory_size=16).
    //   Ignored: autoexec, sdl.fullscreen.
    //
    // An unreadable conf says so and says that nothing was applied:
    //   “Doom” — DOSBOX.CONF could not be read (line 7: expected key=value, got cycles), so none of it was
    //   applied.
    inline QString report(const QString& gameTitle, const QString& confName, const Plan& p)
    {
        const QString title = gameTitle.trimmed();
        const QString head = title.isEmpty() ? confName
                                             : QStringLiteral("“%1” — %2").arg(title, confName);
        if (!p.ok)
            return QStringLiteral("%1 could not be read (%2), so none of it was applied.")
                       .arg(head, p.error);
        if (p.applied.isEmpty() && p.ignored.isEmpty())
            return QStringLiteral("%1 holds no settings, so nothing changed.").arg(head);

        const int total = int(p.applied.size() + p.ignored.size());
        QStringList appliedBits;
        for (const Applied& a : p.applied)
            appliedBits.push_back(QStringLiteral("%1 → %2=%3").arg(a.from, a.option, a.optionValue));
        QStringList ignoredBits;
        for (const Ignored& i : p.ignored) ignoredBits.push_back(i.from);

        QString s = QStringLiteral("%1: %2 of %3 settings applied")
                        .arg(head).arg(p.applied.size()).arg(total);
        if (!appliedBits.isEmpty()) s += QStringLiteral(" (%1)").arg(appliedBits.join(QStringLiteral(", ")));
        s += QLatin1Char('.');
        if (!ignoredBits.isEmpty())
            s += QStringLiteral(" Ignored: %1.").arg(ignoredBits.join(QStringLiteral(", ")));
        return s;
    }

    // The same plan, one line per entry with its REASON, for the log. The user-facing sentence stays short by
    // naming the ignored keys only; a user who wants to know WHY a key was dropped reads these.
    inline QStringList logLines(const Plan& p)
    {
        QStringList out;
        if (!p.ok) { out.push_back(QStringLiteral("conf unreadable: ") + p.error); return out; }
        for (const Applied& a : p.applied)
            out.push_back(QStringLiteral("applied %1=%2 -> %3=%4")
                              .arg(a.from, a.confValue, a.option, a.optionValue));
        for (const Ignored& i : p.ignored)
            out.push_back(QStringLiteral("ignored %1=%2 (%3)").arg(i.from, i.confValue, i.reason));
        return out;
    }

    // ---- MIDI assets (issue #191 part 3) -----------------------------------------------------------------
    // Many DOS games sound dramatically better through a Roland MT-32 or a General MIDI device than through
    // Adlib or the PC speaker, and dosbox-pure supports both — but only if the assets are in the system
    // folder. NOTHING HERE IS EVER FETCHED OR BUNDLED: MT-32 ROMs are copyrighted, and a soundfont is
    // somebody else's licensed content. The app's whole job is BiosCatalog's job and #190's job — name the
    // exact file and the exact folder, and then get out of the way.
    //
    // `files` is ALL-OF, unlike RecipeFirmware::files which is any-of. That difference is real: an MT-32 needs
    // BOTH MT32_CONTROL.ROM and MT32_PCM.ROM, and reporting only the first missing one would send the user
    // back for a second trip.
    struct MidiDevice
    {
        QString     id;      // "gm" | "mt32" — the stored setting value
        QString     label;   // "Roland MT-32"
        QString     value;   // what the core option is set to when the files are present
        QStringList files;   // ALL of these must be in the system folder
        QString     note;    // why EverythingBox cannot provide them ("these ROMs are copyrighted")
    };

    struct MidiSpec
    {
        QString           option;    // "dosbox_pure_midi"
        QList<MidiDevice> devices;
        bool isNull() const { return option.isEmpty() || devices.isEmpty(); }
    };

    inline MidiSpec midiFromJson(const QJsonValue& v)
    {
        MidiSpec s;
        const QJsonObject o = v.toObject();
        s.option = o.value(QStringLiteral("option")).toString().trimmed();
        for (const QJsonValue& dv : o.value(QStringLiteral("devices")).toArray())
        {
            if (!dv.isObject()) continue;
            const QJsonObject d = dv.toObject();
            MidiDevice m;
            m.id    = d.value(QStringLiteral("id")).toString().trimmed().toLower();
            m.label = d.value(QStringLiteral("label")).toString().trimmed();
            m.value = d.value(QStringLiteral("value")).toString().trimmed();
            m.note  = d.value(QStringLiteral("note")).toString().trimmed();
            for (const QJsonValue& fv : d.value(QStringLiteral("files")).toArray())
                if (fv.isString() && !fv.toString().trimmed().isEmpty()) m.files.push_back(fv.toString().trimmed());
            if (m.id.isEmpty() || m.value.isEmpty() || m.files.isEmpty()) continue;
            s.devices.push_back(m);
        }
        return s;
    }

    inline const MidiDevice* midiDevice(const MidiSpec& s, const QString& id)
    {
        const QString want = id.trimmed().toLower();
        if (want.isEmpty()) return nullptr;
        for (const MidiDevice& d : s.devices) if (d.id == want) return &d;
        return nullptr;
    }

    // Which of the device's files are NOT in the firmware folder. `exists` is injected (bare file name in,
    // present?), so this is pure and probe_dosconf drives it against literals.
    inline QStringList missingMidiFiles(const MidiDevice& d, const std::function<bool(const QString&)>& exists)
    {
        QStringList missing;
        if (!exists) return d.files;
        for (const QString& f : d.files) if (!exists(f)) missing.push_back(f);
        return missing;
    }

    // The option to seed for a chosen device whose files are all present. Empty when the device is unknown,
    // is the default ("" / "default"), or is missing a file — in which case the game plays through the
    // core's own default audio and midiMessage() below says why.
    inline QMap<QString, QString> midiOptions(const MidiSpec& s, const QString& deviceId,
                                              const std::function<bool(const QString&)>& exists)
    {
        QMap<QString, QString> out;
        if (s.isNull()) return out;
        const MidiDevice* d = midiDevice(s, deviceId);
        if (!d) return out;
        if (!missingMidiFiles(*d, exists).isEmpty()) return out;
        out.insert(s.option, d->value);
        return out;
    }

    // THE MISSING-ASSET MESSAGE. #190's firmware discipline, word for word: name the file, name the folder,
    // say plainly that the app cannot provide it — and, because a missing MIDI asset is NOT a reason to
    // refuse a launch, say what happens instead.
    //
    //   Roland MT-32 needs files you have to supply: put MT32_CONTROL.ROM and MT32_PCM.ROM in the system
    //   folder (C:\EverythingBox\system). EverythingBox can't download them — these ROMs are copyrighted.
    //   “Doom” will play through its default audio instead.
    inline QString midiMessage(const MidiDevice& d, const QStringList& missing, const QString& folderPath,
                               const QString& gameTitle)
    {
        if (missing.isEmpty()) return QString();
        QString names = missing.first();
        for (int i = 1; i < missing.size(); ++i)
            names += (i == missing.size() - 1 ? QStringLiteral(" and ") : QStringLiteral(", ")) + missing.at(i);
        const QString label = d.label.isEmpty() ? d.id : d.label;
        QString s = QStringLiteral("%1 needs %2 you have to supply: put %3 in the system folder (%4).")
                        .arg(label,
                             missing.size() == 1 ? QStringLiteral("a file") : QStringLiteral("files"),
                             names, folderPath);
        if (!d.note.isEmpty())
            s += QStringLiteral(" EverythingBox can't download %1 — %2.")
                     .arg(missing.size() == 1 ? QStringLiteral("it") : QStringLiteral("them"), d.note);
        const QString title = gameTitle.trimmed();
        s += title.isEmpty() ? QStringLiteral(" The game will play through its default audio instead.")
                             : QStringLiteral(" “%1” will play through its default audio instead.").arg(title);
        return s;
    }
}
