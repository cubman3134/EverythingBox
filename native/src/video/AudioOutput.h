// Audio output options (issue #69): the pure, deterministic map from the user's audio-output preferences to the
// set of mpv option (name, value) pairs the built-in player applies — at player creation and again, live,
// whenever one of the settings changes. Header-only + QtCore-only, so MpvWidget and probe_audioout share ONE
// mapping and no string table can drift between the code and its test (the same arrangement HwDecode.h and
// SubtitleStyle.h use for the hwdec choice and the subtitle style).
//
// WHY these particular options, and the subtleties that matter:
//   * DEVICE: the picker enumerates mpv's `audio-device-list` property (a list of {name, description}) and
//     stores the chosen device's `name`. "Auto" is the default and maps to the id "auto" — mpv's own
//     auto-select, the value `audio-device` holds when nothing is set. A device id is meaningful ONLY on the
//     machine that produced it, which is why the stored key lives in CloudSync's device-local carve-out and is
//     NOT synced (see CloudSync::isDeviceLocalKey). This mapping does not know or care about that; it only
//     turns the stored id into the option string, Auto included.
//   * PASSTHROUGH (bitstream to receiver): `audio-spdif` names the codecs mpv should bitstream untouched to an
//     AV receiver rather than decode to PCM — so the receiver does Dolby/DTS itself. ON emits the codec list;
//     OFF emits an EMPTY string, which is mpv's own default and disables spdif. It is emitted UNCONDITIONALLY
//     (empty when off) so that a live re-apply after turning passthrough off actively clears whatever the
//     previous value set, rather than leaving the codec list stuck on the mpv instance. Note for the UI, not
//     this map: while passthrough is on, controls that require decoding (pitch-corrected speed, the volume
//     filter) do not apply — mpv is handing the untouched bitstream to the receiver.
//   * EXCLUSIVE mode: `audio-exclusive=yes` takes exclusive control of the output device (WASAPI exclusive on
//     Windows, the equivalent elsewhere) for bit-perfect output that bypasses the OS mixer. OFF emits "no",
//     mpv's default (shared mode). Emitted unconditionally for the same reset-on-reapply reason.
//
// The passthrough and exclusive options reconfigure the audio output, so a change may only take full effect
// when mpv next (re)initialises the AO — the device change takes effect live. MpvWidget applies the whole set
// unconditionally, exactly like the subtitle style, so there is never a stale option left on the context.
#pragma once
#include <QString>
#include <QVector>
#include <QPair>

namespace AudioOutput
{
    // One coherent set of audio-output preferences. Defaults mirror mpv's own no-options-set behaviour (auto
    // device, no passthrough, shared/non-exclusive) so a factory-fresh install outputs exactly as mpv would.
    struct Output
    {
        QString device;              // stored audio-device id; "" => Auto (emitted as "auto", mpv's auto-select)
        bool    passthrough = false; // -> audio-spdif: the codec list when on, empty (disabled) when off
        bool    exclusive   = false; // -> audio-exclusive: "yes" when on, "no" (shared) when off
    };

    // The stored default device id (Auto). Kept here so Settings and the two settings builders agree that an
    // empty/absent audio/device means Auto, and that Auto maps to mpv's "auto".
    inline QString autoDeviceId() { return QStringLiteral("auto"); }

    // The codecs bitstreamed to the receiver when passthrough is on. Kept here (not inlined into toMpvOptions)
    // so Settings, the builders and probe_audioout name the SAME list — the "checkbox per codec" UI the issue
    // defers to later will subset this, but v1 is the whole list behind one switch.
    inline QString passthroughCodecs() { return QStringLiteral("ac3,eac3,dts,dts-hd,truehd"); }

    // Pure: an Output -> the ordered list of mpv (option name, value) pairs MpvWidget sets via
    // mpv_set_option_string. Deterministic; no I/O; no dependence on an mpv instance.
    inline QVector<QPair<QString, QString>> toMpvOptions(const Output& o)
    {
        QVector<QPair<QString, QString>> out;
        out.reserve(3);
        // Auto => "auto" (mpv's own auto-select). A stored id => that id verbatim. Trimmed so a stray space in
        // a hand-edited ini does not become a device nobody has.
        const QString dev = o.device.trimmed();
        out << qMakePair(QStringLiteral("audio-device"), dev.isEmpty() ? autoDeviceId() : dev);
        // Passthrough: the codec list when on; EMPTY when off (mpv's default — spdif disabled). Emitted even
        // when off so a live re-apply clears a previously-set list rather than leaving it bitstreaming.
        out << qMakePair(QStringLiteral("audio-spdif"), o.passthrough ? passthroughCodecs() : QString());
        // Exclusive: "yes" grabs the device exclusively (WASAPI exclusive / equivalent); "no" is shared mode.
        out << qMakePair(QStringLiteral("audio-exclusive"), o.exclusive ? QStringLiteral("yes") : QStringLiteral("no"));
        return out;
    }
}
