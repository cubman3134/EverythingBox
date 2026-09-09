// TrickplayPower — the thin, platform-specific answer to "is this machine on mains?", over the pure
// TrickplayIdle predicate (issue #302). Same split as DeviceProfile / DeviceProfileDetect: everything that
// touches an OS lives here, so TrickplayIdle.h stays pure and the CI runner can drive the decision without
// any hardware at all.
//
// WHAT IT CAN ANSWER, honestly, and this list is the point of the file:
//
//   * Windows — GetSystemPowerStatus. `ACLineStatus` 1 is mains and 0 is battery; a machine with NO battery
//     (`BatteryFlag` bit 0x80) is mains whatever the line status says, which is what makes an ordinary
//     desktop answer Mains rather than Unknown. 255 ("unknown") stays Unknown.
//   * Linux — /sys/class/power_supply. A supply of type "Mains" (or "USB"/"USB_PD"…, i.e. anything that is
//     not "Battery") reading `online` = 1 is mains; if the only supplies present are batteries and none of
//     them reports an online mains, that is battery. NO supplies at all — a container, a VM, a server —
//     means the machine has no battery to protect, so it is Mains.
//   * EVERYTHING ELSE — Android, iOS, macOS — answers Unknown. Not "probably mains": Unknown. Android would
//     need a JNI hop into BatteryManager and iOS a UIDevice call, neither of which exists in this tree, and
//     a courtesy job is not worth inventing a half-right answer for. TrickplayIdle::evaluate refuses on
//     Unknown, so on those platforms idle generation simply never runs — and #85's between-playbacks trigger,
//     which is a consequence of the user having opened a file, is completely unaffected.
//
// IT IS CHEAP AND IT IS NOT CACHED. Power state is the one input here that changes while the app is running —
// somebody unplugs a laptop — so memoising it would defeat the whole rule. GetSystemPowerStatus is a syscall
// and the sysfs read is a couple of small files; both are polled at the walk's cadence (seconds), not per
// frame.
#pragma once
#include "TrickplayIdle.h"

namespace TrickplayPower
{
    // This machine's power source right now, or Unknown where we cannot tell (see the list above).
    TrickplayIdle::Power current();
}
