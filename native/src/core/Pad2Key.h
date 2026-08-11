// Pad-to-keyboard profiles (issue #105) — the PURE heart. A keyboard-only game (an MS-DOS title, a keyboard-era
// PC game) launched from the couch is a dead end: there is no keyboard to reach. A pad2key profile synthesises
// keystrokes from pad input — press A, the game sees Enter — so a slice of the catalog that was "listed but
// unplayable from the couch" becomes usable. This header is the model + the deterministic translation the
// runtime injector calls; it is the unit probe_pad2key mutation-tests. QtCore-only, header-only, NO SDL, NO OS
// call — the runtime (Pad2KeyRuntime) reads the pad via the existing SDL layer and turns the KeyEvents this
// produces into OS key events (SendInput on Windows), and the store (Pad2KeyStore) persists a per-game profile.
//
// WHY THE HYSTERESIS FOLD IS A SIBLING PURE FUNCTION, NOT FOLDED INTO translate(). An analog stick must engage
// its mapped arrow past an ENTER threshold and only release below a LOWER EXIT threshold, or a stick resting
// near the threshold chatters the key on and off. That is inherently stateful: whether the axis is "engaged"
// while sitting in the band between EXIT and ENTER cannot be decided from one sample — it depends on whether it
// last crossed ENTER (still engaged) or last fell below EXIT (not). A single stateless sample→bool cannot
// express it. So the engaged bit is resolved by resolveStick(magnitude, prevEngaged, …) — pure, and threaded
// across frames by the CALLER — and stored INTO PadState.down[stickControl]. translate() is then a pure edge
// detector over prev/cur .down[] for every control alike (digital or stick-resolved): a false→true emits the
// mapped key(s) DOWN, a true→false emits them UP. Both functions are pure and both are mutation-tested; the
// probe threads resolveStick frame by frame exactly as the runtime does, so the chatter guard is real, and a
// key left DOWN when the stick falls straight through the band (the stuck-key footgun) is provably released.
#pragma once
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>
#include <cstdlib>

namespace pad2key
{
    // ---- the abstract pad control set. Digital buttons + the two analog sticks split into four directions each.
    // Stick directions are analog: their PadState.down bit is the RESOLVED engaged state (resolveStick), never a
    // raw sample. Order is stable — translate iterates it in this order so its output is deterministic regardless
    // of the profile map's hash order.
    enum class Control {
        DpadUp, DpadDown, DpadLeft, DpadRight,
        A, B, X, Y,
        L1, R1, L2, R2,
        Start, Select,
        LStickUp, LStickDown, LStickLeft, LStickRight,
        RStickUp, RStickDown, RStickLeft, RStickRight,
        Count
    };
    inline constexpr int kControlCount = static_cast<int>(Control::Count);

    inline bool isStick(Control c)
    {
        return c >= Control::LStickUp && c <= Control::RStickRight;
    }

    // ---- OS-agnostic key tokens. The runtime maps each to the platform virtual-key; this header never names a
    // VK. Kept deliberately small (letters, digits, the navigation/edit cluster, the three modifiers, F1–F12) —
    // the keys a keyboard-only game actually binds. A combo is an ordered list of these (Ctrl+Enter).
    enum class Key {
        None = 0,
        A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
        D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,
        Up, Down, Left, Right,
        Enter, Esc, Space, Tab, Backspace, Delete, Insert, Home, End, PageUp, PageDown,
        Ctrl, Shift, Alt,
        F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12
    };

    using KeyChord = QVector<Key>;   // one control → one or more keys (a combo); order is press order

    struct KeyEvent { Key key; bool down; };   // what translate emits: this key goes down (true) / up (false)

    // A frame of pad input. down[c] for a digital control is "held this frame"; for a stick control it is the
    // RESOLVED engaged state after resolveStick has been applied by the caller/probe. Default-constructed = all
    // released, which is the correct neutral "prev" for the very first frame.
    struct PadState {
        std::array<bool, kControlCount> down{};   // value-initialised: every control released
        bool get(Control c) const { return down[static_cast<size_t>(c)]; }
        void set(Control c, bool v) { down[static_cast<size_t>(c)] = v; }
    };

    // A profile: an OS-agnostic name plus the control→chord map. Serialisable (rides Pad2KeyStore / the sync
    // category). An unmapped control is simply absent from the map — translate emits nothing for it.
    struct Profile {
        QString name;
        QHash<Control, KeyChord> map;
        bool isEmpty() const { return map.isEmpty(); }
    };

    // ---- analog hysteresis. magnitude is |axis| in ONE direction, 0..32767 (the runtime rectifies the raw
    // signed SDL axis to the positive side per direction before calling this). ENTER=16000 (~49% deflection)
    // engages; EXIT=8000 (~24%) releases; the band between them holds the previous state so a stick resting near
    // the line does not chatter. Pure and total: given the same (magnitude, prevEngaged) it always returns the
    // same bit, so the probe drives every transition by threading prevEngaged exactly as the runtime does.
    inline constexpr int kAxisEnter = 16000;
    inline constexpr int kAxisExit  = 8000;

    inline bool resolveStick(int magnitude, bool prevEngaged,
                             int enter = kAxisEnter, int exit = kAxisExit)
    {
        if (prevEngaged) return magnitude > exit;    // engaged: stay until it drops to/under EXIT
        return magnitude >= enter;                   // not engaged: engage only at/over ENTER
    }

    // ---- the load-bearing translation. Pure edge detection over prev/cur .down[], mapped through the profile.
    // For every control IN ENUM ORDER (deterministic): unmapped → nothing; no edge → nothing; false→true emits
    // its chord DOWN in press order; true→false emits its chord UP in REVERSE order (so Ctrl+Enter releases
    // Enter then Ctrl, the mirror of pressing Ctrl then Enter). No state is kept here — prev and cur are the
    // whole state — so a held button emits its key once on the press and nothing more until it releases.
    inline QVector<KeyEvent> translate(const PadState& prev, const PadState& cur, const Profile& p)
    {
        QVector<KeyEvent> out;
        for (int i = 0; i < kControlCount; ++i)
        {
            const Control c = static_cast<Control>(i);
            const auto it = p.map.constFind(c);
            if (it == p.map.constEnd() || it.value().isEmpty()) continue;   // unmapped control → nothing
            const bool was = prev.get(c);
            const bool now = cur.get(c);
            if (was == now) continue;                                       // no edge → nothing
            const KeyChord& chord = it.value();
            if (now)
                for (int k = 0; k < chord.size(); ++k) out.push_back({ chord[k], true });   // press: forward
            else
                for (int k = chord.size() - 1; k >= 0; --k) out.push_back({ chord[k], false }); // release: reverse
        }
        return out;
    }

    // ---- stable string tokens for JSON (control + key). Independent of the enum's integer values, so a token
    // never shifts if the enum is reordered. ------------------------------------------------------------------
    inline QString controlToken(Control c)
    {
        switch (c) {
            case Control::DpadUp:      return QStringLiteral("DpadUp");
            case Control::DpadDown:    return QStringLiteral("DpadDown");
            case Control::DpadLeft:    return QStringLiteral("DpadLeft");
            case Control::DpadRight:   return QStringLiteral("DpadRight");
            case Control::A:           return QStringLiteral("A");
            case Control::B:           return QStringLiteral("B");
            case Control::X:           return QStringLiteral("X");
            case Control::Y:           return QStringLiteral("Y");
            case Control::L1:          return QStringLiteral("L1");
            case Control::R1:          return QStringLiteral("R1");
            case Control::L2:          return QStringLiteral("L2");
            case Control::R2:          return QStringLiteral("R2");
            case Control::Start:       return QStringLiteral("Start");
            case Control::Select:      return QStringLiteral("Select");
            case Control::LStickUp:    return QStringLiteral("LStickUp");
            case Control::LStickDown:  return QStringLiteral("LStickDown");
            case Control::LStickLeft:  return QStringLiteral("LStickLeft");
            case Control::LStickRight: return QStringLiteral("LStickRight");
            case Control::RStickUp:    return QStringLiteral("RStickUp");
            case Control::RStickDown:  return QStringLiteral("RStickDown");
            case Control::RStickLeft:  return QStringLiteral("RStickLeft");
            case Control::RStickRight: return QStringLiteral("RStickRight");
            default:                   return QString();
        }
    }
    inline Control controlFromToken(const QString& s, bool* ok = nullptr)
    {
        for (int i = 0; i < kControlCount; ++i)
            if (controlToken(static_cast<Control>(i)) == s) { if (ok) *ok = true; return static_cast<Control>(i); }
        if (ok) *ok = false;
        return Control::Count;
    }

    inline QString keyToken(Key k)
    {
        // Letters and digits are computed off their contiguous runs; the rest are named.
        if (k >= Key::A && k <= Key::Z)
            return QString(QChar('A' + (static_cast<int>(k) - static_cast<int>(Key::A))));
        if (k >= Key::D0 && k <= Key::D9)
            return QString(QChar('0' + (static_cast<int>(k) - static_cast<int>(Key::D0))));
        if (k >= Key::F1 && k <= Key::F12)
            return QStringLiteral("F") + QString::number(static_cast<int>(k) - static_cast<int>(Key::F1) + 1);
        switch (k) {
            case Key::Up:        return QStringLiteral("Up");
            case Key::Down:      return QStringLiteral("Down");
            case Key::Left:      return QStringLiteral("Left");
            case Key::Right:     return QStringLiteral("Right");
            case Key::Enter:     return QStringLiteral("Enter");
            case Key::Esc:       return QStringLiteral("Esc");
            case Key::Space:     return QStringLiteral("Space");
            case Key::Tab:       return QStringLiteral("Tab");
            case Key::Backspace: return QStringLiteral("Backspace");
            case Key::Delete:    return QStringLiteral("Delete");
            case Key::Insert:    return QStringLiteral("Insert");
            case Key::Home:      return QStringLiteral("Home");
            case Key::End:       return QStringLiteral("End");
            case Key::PageUp:    return QStringLiteral("PageUp");
            case Key::PageDown:  return QStringLiteral("PageDown");
            case Key::Ctrl:      return QStringLiteral("Ctrl");
            case Key::Shift:     return QStringLiteral("Shift");
            case Key::Alt:       return QStringLiteral("Alt");
            default:             return QString();   // Key::None and anything unhandled
        }
    }
    inline Key keyFromToken(const QString& s)
    {
        if (s.size() == 1)
        {
            const QChar ch = s.at(0).toUpper();
            if (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
                return static_cast<Key>(static_cast<int>(Key::A) + (ch.unicode() - 'A'));
            if (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
                return static_cast<Key>(static_cast<int>(Key::D0) + (ch.unicode() - '0'));
        }
        if (s.size() >= 2 && (s.at(0) == QLatin1Char('F') || s.at(0) == QLatin1Char('f')))
        {
            bool numOk = false;
            const int n = s.mid(1).toInt(&numOk);
            if (numOk && n >= 1 && n <= 12) return static_cast<Key>(static_cast<int>(Key::F1) + (n - 1));
        }
        if (s == QLatin1String("Up"))        return Key::Up;
        if (s == QLatin1String("Down"))      return Key::Down;
        if (s == QLatin1String("Left"))      return Key::Left;
        if (s == QLatin1String("Right"))     return Key::Right;
        if (s == QLatin1String("Enter"))     return Key::Enter;
        if (s == QLatin1String("Esc"))       return Key::Esc;
        if (s == QLatin1String("Space"))     return Key::Space;
        if (s == QLatin1String("Tab"))       return Key::Tab;
        if (s == QLatin1String("Backspace")) return Key::Backspace;
        if (s == QLatin1String("Delete"))    return Key::Delete;
        if (s == QLatin1String("Insert"))    return Key::Insert;
        if (s == QLatin1String("Home"))      return Key::Home;
        if (s == QLatin1String("End"))       return Key::End;
        if (s == QLatin1String("PageUp"))    return Key::PageUp;
        if (s == QLatin1String("PageDown"))  return Key::PageDown;
        if (s == QLatin1String("Ctrl"))      return Key::Ctrl;
        if (s == QLatin1String("Shift"))     return Key::Shift;
        if (s == QLatin1String("Alt"))       return Key::Alt;
        return Key::None;
    }

    // ---- Profile <-> JSON. One canonical spelling: controls serialise in ENUM order, a single-key chord as a
    // bare string, a multi-key chord as an array, an unknown/None key dropped. So fromJson(toJson(p)) == p, and
    // two devices that authored the same profile store byte-identical records.
    inline QJsonObject toJson(const Profile& p)
    {
        QJsonObject o;
        if (!p.name.isEmpty()) o.insert(QStringLiteral("name"), p.name);
        QJsonObject m;
        for (int i = 0; i < kControlCount; ++i)   // enum order → stable bytes
        {
            const Control c = static_cast<Control>(i);
            const auto it = p.map.constFind(c);
            if (it == p.map.constEnd() || it.value().isEmpty()) continue;
            const KeyChord& chord = it.value();
            if (chord.size() == 1)
                m.insert(controlToken(c), keyToken(chord.first()));
            else
            {
                QJsonArray arr;
                for (Key k : chord) arr.append(keyToken(k));
                m.insert(controlToken(c), arr);
            }
        }
        o.insert(QStringLiteral("map"), m);
        return o;
    }
    inline Profile fromJson(const QJsonObject& o)
    {
        Profile p;
        p.name = o.value(QStringLiteral("name")).toString();
        const QJsonObject m = o.value(QStringLiteral("map")).toObject();
        for (auto it = m.begin(); it != m.end(); ++it)
        {
            bool ok = false;
            const Control c = controlFromToken(it.key(), &ok);
            if (!ok) continue;                                 // an unknown control token is dropped
            KeyChord chord;
            if (it.value().isArray())
            {
                for (const QJsonValue& v : it.value().toArray())
                {
                    const Key k = keyFromToken(v.toString());
                    if (k != Key::None) chord.push_back(k);
                }
            }
            else
            {
                const Key k = keyFromToken(it.value().toString());
                if (k != Key::None) chord.push_back(k);
            }
            if (!chord.isEmpty()) p.map.insert(c, chord);       // a control that mapped to nothing usable is absent
        }
        return p;
    }

    // ---- per-system default profiles. The issue's headline: a sensible MS-DOS default (d-pad→arrows, A→Enter,
    // B→Esc) covers a surprising fraction of the keyboard-only catalog. defaultProfile(systemId) returns it for
    // "msdos" and an EMPTY profile (no synthesis) for anything else — a system with no default is one pad2key
    // does nothing for unless the user authors a profile. Also offered for the PC/DOS standalone tier by id.
    inline Profile dosDefaultProfile()
    {
        Profile p;
        p.name = QStringLiteral("DOS default");
        p.map.insert(Control::DpadUp,    { Key::Up });
        p.map.insert(Control::DpadDown,  { Key::Down });
        p.map.insert(Control::DpadLeft,  { Key::Left });
        p.map.insert(Control::DpadRight, { Key::Right });
        // The left stick drives the same arrows (stick-only pads, and it reads more naturally than the d-pad for
        // movement). Runtime resolves these via resolveStick before translate.
        p.map.insert(Control::LStickUp,    { Key::Up });
        p.map.insert(Control::LStickDown,  { Key::Down });
        p.map.insert(Control::LStickLeft,  { Key::Left });
        p.map.insert(Control::LStickRight, { Key::Right });
        p.map.insert(Control::A,     { Key::Enter });
        p.map.insert(Control::B,     { Key::Esc });
        p.map.insert(Control::Start, { Key::Enter });
        p.map.insert(Control::Select,{ Key::Esc });
        p.map.insert(Control::X, { Key::Space });   // X → Space (jump / confirm in many DOS games)
        return p;
    }

    inline Profile defaultProfile(const QString& systemId)
    {
        if (systemId == QLatin1String("msdos") || systemId == QLatin1String("dos") || systemId == QLatin1String("pc"))
            return dosDefaultProfile();
        return Profile{};   // no per-system default → pad2key synthesises nothing unless a profile is authored
    }
}
