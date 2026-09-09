#include "TrickplayPower.h"

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#endif

namespace TrickplayPower
{

TrickplayIdle::Power current()
{
#if defined(Q_OS_WIN)
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s)) return TrickplayIdle::Power::Unknown;
    // "No system battery" (0x80) is the ordinary desktop, and it is mains by construction — there is nothing
    // to run down. Checked BEFORE ACLineStatus because some desktop firmware reports the line status as 255.
    if (s.BatteryFlag != 255 && (s.BatteryFlag & 128)) return TrickplayIdle::Power::Mains;
    if (s.ACLineStatus == 1) return TrickplayIdle::Power::Mains;
    if (s.ACLineStatus == 0) return TrickplayIdle::Power::Battery;
    return TrickplayIdle::Power::Unknown;   // 255: the machine will not say, so neither do we
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    QDir root(QStringLiteral("/sys/class/power_supply"));
    if (!root.exists()) return TrickplayIdle::Power::Unknown;
    const QFileInfoList supplies = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    bool sawBattery = false;
    for (const QFileInfo& fi : supplies)
    {
        QFile tf(fi.absoluteFilePath() + QStringLiteral("/type"));
        if (!tf.open(QIODevice::ReadOnly)) continue;
        const QByteArray type = tf.readAll().trimmed();
        if (type == "Battery") { sawBattery = true; continue; }
        // Anything that is not a battery is a supply: Mains, USB, USB_PD, ... It counts only when it says it
        // is actually plugged in.
        QFile of(fi.absoluteFilePath() + QStringLiteral("/online"));
        if (!of.open(QIODevice::ReadOnly)) continue;
        if (of.readAll().trimmed() == "1") return TrickplayIdle::Power::Mains;
    }
    // A battery, and no supply online: unplugged. No batteries and no supplies at all (a container, a VM, a
    // headless server) is a machine with nothing to run down, which is mains for our purposes.
    return sawBattery ? TrickplayIdle::Power::Battery : TrickplayIdle::Power::Mains;
#else
    // Android / iOS / macOS: see the header. Unknown, deliberately, and TrickplayIdle::evaluate refuses on it.
    return TrickplayIdle::Power::Unknown;
#endif
}

} // namespace TrickplayPower
