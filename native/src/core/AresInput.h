// ares input seeding (the pure heart) — translate a connected pad's SDL gamepad mapping string into the
// bindings ares reads out of its own settings.bml. ares ships with NO input bindings whatsoever: there is no
// auto-map, no keyboard default and no first-run assignment anywhere in its desktop-ui, so a fresh install
// boots a game that cannot be controlled until the user maps sixteen controls by hand. This is the standalone
// twin of the melonDS seed EmulatorManager already performs for the same reason.
//
// PURE, header-only, QtCore-only, NO SDL, NO disk. The write side (EmulatorManager::prepareControllerConfig)
// enumerates live pads via SDL, calls ControllerSeats::assignSeats, and seeds settingsBml() ONLY when
// needsSeed() says no VirtualPad assignment exists — so a user's own mapping is never clobbered. Keeping this
// pure is what lets probe_aresinput pin the whole translation against real gamecontrollerdb lines with no SDL,
// no disk and no live emulator.
//
// THE FORMAT (verified against ares-emulator/ares@master, v148):
//   * settings.bml is BML written by BML::serialize(*this, " ") — two spaces of indent per depth,
//     "name: value", LF newlines.
//   * Each virtual-pad input lives at VirtualPad{1..5}/{name}, where {name} is the input's DISPLAY name
//     pushed through .replace(" ", ".").replace("(", ".").replace(")", "") — so "A (South)" is stored as
//     "A..South" and "L-Stick (Click)" as "L-Stick..Click" (desktop-ui/settings/settings.cpp).
//   * An assignment is "<identity>/<slot>/<groupID>/<inputID>[/<qualifier>]" (InputMapping::bind).
//     `identity` is the SDL GUID string and `slot` disambiguates two pads reporting the SAME GUID
//     (ruby/input/joypad/sdl.cpp sets identifier = {identity, "/", slot}). We always emit slot 0: seats are
//     distinguished by their VirtualPad NUMBER, and ares' own enumeration order is not observable from here.
//   * Group ids are nall's HID::Joypad::GroupID — Axis 0, Hat 1, Trigger 2, Button 3 (nall/nall/hid.hpp).
//     The SDL joypad driver only ever populates Axis, Hat and Button.
//   * inputID is the RAW joystick index, because that driver enumerates SDL_GetNumJoystickAxes/Hats/Buttons
//     rather than the gamepad abstraction. Which is exactly why this translation reads the pad's own SDL
//     mapping string instead of assuming a layout: on a "4Play Adapter" the A button is b1 and X is b0.
//   * Each SDL hat becomes TWO ares hat inputs: 2H is X (LEFT -32767, RIGHT +32767) and 2H+1 is Y
//     (UP -32767, DOWN +32767), so up/left take the Lo qualifier and down/right take Hi.
//   * InputDigital::value() thresholds a non-Button group at < -16384 (Lo) / > +16384 (Hi), so a DIGITAL
//     control bound to a hat or to a trigger axis works.
//
// NOT SEEDED, deliberately: Rumble (its assignment needs a raw input to hang a /Rumble qualifier off, which
// the SDL mapping string does not name) and ares' hotkeys (exit is already EverythingBox's job — the launcher
// asks the process to close and force-kills it after a grace period).
#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include "ControllerSeats.h"

namespace AresInput
{
    // nall HID::Joypad::GroupID.
    enum Group { GroupAxis = 0, GroupHat = 1, GroupTrigger = 2, GroupButton = 3 };

    // One seeded line: `key` is the LEAF settings name ("Pad.Up"); settingsBml supplies the VirtualPadN parent.
    struct Binding
    {
        QString key;
        QString value;
        bool operator==(const Binding& o) const { return key == o.key && value == o.value; }
    };

    // ---- internal: one SDL mapping value ("b3", "a0", "-a1", "a4~", "h0.1") resolved to a raw input -------
    struct RawRef
    {
        bool valid = false;
        int  group = GroupButton;
        int  input = 0;
        int  hatMask = 0;   // hats only: 1 up, 2 right, 4 down, 8 left
    };

    // Parse "a:b1,leftx:a0,dpup:h0.1,platform:Windows," (after the leading GUID and name fields) into a map.
    inline QHash<QString, QString> parseMapping(const QString& sdlMapping)
    {
        QHash<QString, QString> out;
        const QStringList fields = sdlMapping.split(QLatin1Char(','), Qt::SkipEmptyParts);
        // Field 0 is the GUID and field 1 the device name; neither is a key:value pair.
        for (int i = 2; i < fields.size(); ++i)
        {
            const int colon = fields[i].indexOf(QLatin1Char(':'));
            if (colon <= 0) continue;
            const QString k = fields[i].left(colon).trimmed();
            const QString v = fields[i].mid(colon + 1).trimmed();
            if (k.isEmpty() || v.isEmpty() || k == QLatin1String("platform")) continue;
            out.insert(k, v);
        }
        return out;
    }

    inline RawRef refFor(const QHash<QString, QString>& map, const char* sdlName)
    {
        RawRef r;
        const QString v = map.value(QLatin1String(sdlName));
        if (v.isEmpty()) return r;
        // SDL decorates an axis with a half-axis prefix (+/-) and/or an inversion suffix (~). Neither changes
        // WHICH raw axis it is, and the direction we want is decided per key below, so strip them.
        QString s = v;
        while (!s.isEmpty() && (s[0] == QLatin1Char('+') || s[0] == QLatin1Char('-'))) s.remove(0, 1);
        while (s.endsWith(QLatin1Char('~'))) s.chop(1);
        if (s.size() < 2) return r;
        const QChar kind = s[0];
        const QString rest = s.mid(1);
        bool ok = false;
        if (kind == QLatin1Char('b'))
        {
            const int n = rest.toInt(&ok);
            if (!ok || n < 0) return r;
            r.valid = true; r.group = GroupButton; r.input = n;
        }
        else if (kind == QLatin1Char('a'))
        {
            const int n = rest.toInt(&ok);
            if (!ok || n < 0) return r;
            r.valid = true; r.group = GroupAxis; r.input = n;
        }
        else if (kind == QLatin1Char('h'))
        {
            const int dot = rest.indexOf(QLatin1Char('.'));
            if (dot <= 0) return r;
            bool ok2 = false;
            const int hat  = rest.left(dot).toInt(&ok);
            const int mask = rest.mid(dot + 1).toInt(&ok2);
            if (!ok || !ok2 || hat < 0) return r;
            // Hat H occupies ares inputs 2H (X) and 2H+1 (Y).
            const bool vertical = (mask == 1 || mask == 4);
            r.valid = true; r.group = GroupHat; r.input = hat * 2 + (vertical ? 1 : 0); r.hatMask = mask;
        }
        return r;
    }

    // The qualifier a DIGITAL control needs for this raw input: none for a button, Lo/Hi from the hat
    // direction, Hi for an axis (a trigger rests low and rises).
    inline QString digitalQualifier(const RawRef& r)
    {
        if (r.group == GroupButton) return QString();
        if (r.group == GroupHat)
            return (r.hatMask == 1 || r.hatMask == 8) ? QStringLiteral("Lo") : QStringLiteral("Hi");
        return QStringLiteral("Hi");
    }

    inline QString assignment(const QString& guid, const RawRef& r, const QString& qualifier)
    {
        QString s = guid + QStringLiteral("/0/") + QString::number(r.group) + QLatin1Char('/')
                  + QString::number(r.input);
        if (!qualifier.isEmpty()) s += QLatin1Char('/') + qualifier;
        return s;
    }

    // ---- pure: every binding one pad contributes, in a stable order --------------------------------------
    // A control the pad's mapping does not declare simply gets NO binding — degrade, never guess. An empty
    // mapping string (SDL knows the device but has no gamepad profile for it) yields nothing at all.
    inline QVector<Binding> bindingsFor(const ControllerSeats::PadInfo& pad)
    {
        QVector<Binding> out;
        if (pad.sdlMapping.isEmpty() || pad.guid.isEmpty()) return out;
        const QHash<QString, QString> map = parseMapping(pad.sdlMapping);
        if (map.isEmpty()) return out;

        // Digital controls: ares settings leaf <- SDL control name.
        struct D { const char* key; const char* sdl; };
        static const D kDigital[] = {
            { "Pad.Up",         "dpup"          }, { "Pad.Down",       "dpdown"        },
            { "Pad.Left",       "dpleft"        }, { "Pad.Right",      "dpright"       },
            { "Select",         "back"          }, { "Start",          "start"         },
            { "A..South",       "a"             }, { "B..East",        "b"             },
            { "X..West",        "x"             }, { "Y..North",       "y"             },
            { "L-Bumper",       "leftshoulder"  }, { "R-Bumper",       "rightshoulder" },
            { "L-Trigger",      "lefttrigger"   }, { "R-Trigger",      "righttrigger"  },
            { "L-Stick..Click", "leftstick"     }, { "R-Stick..Click", "rightstick"    },
        };
        for (const D& d : kDigital)
        {
            const RawRef r = refFor(map, d.sdl);
            if (!r.valid) continue;
            out.push_back(Binding{ QLatin1String(d.key), assignment(pad.guid, r, digitalQualifier(r)) });
        }

        // Analog stick directions: one axis, split into its two halves. SDL reports X negative = left and
        // Y negative = up, matching ares' Lo/Hi. A pad that reports a stick as buttons contributes nothing
        // here (r.group != GroupAxis), which is correct: a digital stick is not an analog source.
        struct A { const char* key; const char* sdl; const char* qual; };
        static const A kAnalog[] = {
            { "L-Left",  "leftx",  "Lo" }, { "L-Right", "leftx",  "Hi" },
            { "L-Up",    "lefty",  "Lo" }, { "L-Down",  "lefty",  "Hi" },
            { "R-Left",  "rightx", "Lo" }, { "R-Right", "rightx", "Hi" },
            { "R-Up",    "righty", "Lo" }, { "R-Down",  "righty", "Hi" },
        };
        for (const A& a : kAnalog)
        {
            const RawRef r = refFor(map, a.sdl);
            if (!r.valid || r.group != GroupAxis) continue;
            out.push_back(Binding{ QLatin1String(a.key), assignment(pad.guid, r, QLatin1String(a.qual)) });
        }
        return out;
    }

    // ---- pure: the settings.bml body to seed for a whole seat list ---------------------------------------
    // Seat n becomes VirtualPad{n+1}. A seat whose pad contributes no binding is skipped entirely, and an
    // empty seat list yields an empty body — the caller then seeds no file at all.
    inline QByteArray settingsBml(const QVector<ControllerSeats::Seat>& seats)
    {
        QByteArray out;
        for (const ControllerSeats::Seat& s : seats)
        {
            const QVector<Binding> bs = bindingsFor(s.pad);
            if (bs.isEmpty()) continue;
            out += QStringLiteral("VirtualPad%1\n").arg(s.index + 1).toUtf8();
            for (const Binding& b : bs)
                out += "  " + b.key.toUtf8() + ": " + b.value.toUtf8() + "\n";
        }
        return out;
    }

    // ---- pure: may we seed over this existing settings.bml? ----------------------------------------------
    // True only when NO VirtualPad input carries a non-empty assignment: an absent file, or one ares wrote
    // itself before we ever seeded (it persists every key with an empty value when nothing is mapped). Any
    // single real assignment — a user's own mapping — makes this false and the file is left untouched.
    inline bool needsSeed(const QByteArray& existingSettingsBml)
    {
        const QStringList lines = QString::fromUtf8(existingSettingsBml).split(QLatin1Char('\n'));
        bool inPad = false;
        for (const QString& raw : lines)
        {
            const QString line = raw.trimmed();
            if (line.isEmpty()) continue;
            const bool topLevel = !raw.startsWith(QLatin1Char(' ')) && !raw.startsWith(QLatin1Char('\t'));
            if (topLevel) { inPad = line.startsWith(QStringLiteral("VirtualPad")); continue; }
            if (!inPad) continue;
            const int colon = line.indexOf(QLatin1Char(':'));
            if (colon >= 0 && !line.mid(colon + 1).trimmed().isEmpty()) return false;
        }
        return true;
    }

    // ---- pure: fold a seed into an existing settings.bml -------------------------------------------------
    // MUST NOT be a plain append. ares resolves a settings path with Markup::Node::operator[], which is
    // _lookup -> _find(path)[0] — the FIRST match (nall/nall/string/markup/find.hpp). A second VirtualPad1
    // block appended after one ares already wrote is therefore ignored on load, and Settings::save's
    // operator()(path).setValue() writes back into the FIRST block — so the seed would read perfectly in the
    // file and do nothing at all. Every pre-existing top-level VirtualPad{N} block is dropped (including one
    // for a pad we are not seeding this time, so the file cannot accumulate stale controllers) and the seed
    // appended, leaving exactly one block per seated pad. Every non-pad setting is preserved byte-for-byte.
    // Only ever called behind needsSeed(), so no real assignment can be discarded here.
    inline QByteArray mergeSettingsBml(const QByteArray& existing, const QByteArray& seed)
    {
        if (existing.isEmpty()) return seed;

        QByteArray kept;
        bool dropping = false;
        const QList<QByteArray> lines = existing.split('\n');
        for (int i = 0; i < lines.size(); ++i)
        {
            const QByteArray& raw = lines[i];
            // A trailing "\n" splits to a final empty element; don't re-emit it as a blank line.
            if (i == lines.size() - 1 && raw.isEmpty()) break;
            const bool topLevel = !raw.startsWith(' ') && !raw.startsWith('\t') && !raw.trimmed().isEmpty();
            if (topLevel)
                dropping = raw.trimmed().startsWith("VirtualPad");
            if (dropping) continue;
            kept += raw;
            kept += '\n';
        }
        return kept + seed;
    }
}
