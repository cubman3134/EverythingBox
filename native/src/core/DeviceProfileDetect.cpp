#include "DeviceProfileDetect.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QSettings>
#include <QThread>

#if defined(Q_OS_ANDROID)
#include <sys/system_properties.h>
#include <sys/sysinfo.h>
#endif

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// Shares the portable everythingbox.ini with the other stores (same AppPaths::dataDir() posture). We only ever
// touch the "device/" prefix here, which CloudSync classifies device-local.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

namespace {

const QLatin1String kCacheKey("device/profile");            // resolved+cached detected Kind token
const QLatin1String kOverrideKey("device/profileOverride"); // manual pin (empty = detection)

bool                   mMemoBuilt = false;
DeviceProfile::Profile mMemoDetected;   // detection result (override-independent)

// ---- platform inputs (the only OS-touching code; DeviceProfile.h stays pure) -----------------------------

// Windows DMI product string. The registry SystemProductName under HKLM\...\BIOS is the reliable, WMI-free
// source Valve/Asus/Lenovo populate ("Jupiter", "Galileo", "ROG Ally RC71L", "83E1"). Read via QSettings'
// native (registry) backend — still QtCore, no COM.
QString dmiProductString()
{
#if defined(Q_OS_WIN)
    QSettings bios(QStringLiteral("HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\BIOS"),
                   QSettings::NativeFormat);
    QString v = bios.value(QStringLiteral("SystemProductName")).toString().trimmed();
    if (!v.isEmpty()) return v;
    // Fallback: some OEMs leave SystemProductName generic ("System Product Name") and carry the real one in
    // BaseBoardProduct.
    return bios.value(QStringLiteral("BaseBoardProduct")).toString().trimmed();
#else
    return QString();
#endif
}

// Android ro.product identity (best-effort). The concrete handheld list is not yet recognised by name (see
// DeviceProfile::fromAndroidProduct), but we still gather the string so a future list needs no glue change.
QString androidProductString()
{
#if defined(Q_OS_ANDROID)
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("ro.product.device", buf) > 0 && buf[0]) return QString::fromUtf8(buf);
    if (__system_property_get("ro.product.model", buf) > 0 && buf[0])  return QString::fromUtf8(buf);
#endif
    return QString();
}

// A GPU renderer / adapter description string, best-effort and WMI-free. On Windows the display adapter's
// DriverDesc in the registry is a cheap proxy (mainly to catch "Microsoft Basic Render Driver" = no real GPU);
// elsewhere we leave it empty and let RAM/cores drive the tier.
QString gpuRendererString()
{
#if defined(Q_OS_WIN)
    QSettings vid(QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Class\\"
                                 "{4d36e968-e325-11ce-bfc1-08002be10318}\\0000"),
                  QSettings::NativeFormat);
    return vid.value(QStringLiteral("DriverDesc")).toString().trimmed();
#else
    return QString();
#endif
}

double totalRamGB()
{
#if defined(Q_OS_WIN)
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        return double(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    return 0.0;
#elif defined(Q_OS_ANDROID)
    struct sysinfo si;
    if (sysinfo(&si) == 0)
        return double(si.totalram) * double(si.mem_unit) / (1024.0 * 1024.0 * 1024.0);
    return 0.0;
#else
    return 0.0;
#endif
}

// Run the full detection pass (platform inputs -> pure identify). No disk/ini writes here.
DeviceProfile::Profile runDetection()
{
    DeviceProfile::Capability cap;
    cap.gpuRenderer  = gpuRendererString();
    cap.logicalCores = QThread::idealThreadCount();   // -1 if undeterminable; identify() treats <=4 as Low-ish
    if (cap.logicalCores < 0) cap.logicalCores = 0;
    cap.ramGB        = totalRamGB();
    return DeviceProfile::identify(dmiProductString(), androidProductString(), cap);
}

// Resolve the detected profile (override-independent), using the cache when present, running detection and
// caching otherwise. Memoised in-process.
void ensureMemo()
{
    if (mMemoBuilt) return;
    const QString cached = store().value(kCacheKey).toString().trimmed();
    if (!cached.isEmpty() && DeviceProfile::kindFromToken(cached) != DeviceProfile::Kind::Unknown)
    {
        mMemoDetected = DeviceProfile::profileFromToken(cached);
    }
    else
    {
        mMemoDetected = runDetection();
        // Cache even an Unknown result token so a machine with no opinion is not re-probed every launch; the
        // token is device-local (device/ prefix).
        store().setValue(kCacheKey, DeviceProfile::kindToken(mMemoDetected.kind));
        store().sync();
    }
    mMemoBuilt = true;
}

} // namespace

DeviceProfile::Profile DeviceProfileDetect::detected()
{
    ensureMemo();
    return mMemoDetected;
}

DeviceProfile::Profile DeviceProfileDetect::active()
{
    const QString ov = store().value(kOverrideKey).toString().trimmed();
    if (!ov.isEmpty() && DeviceProfile::kindFromToken(ov) != DeviceProfile::Kind::Unknown)
        return DeviceProfile::profileFromToken(ov);   // a valid pin wins; detection irrelevant
    return detected();
}

EmuGfx::Settings DeviceProfileDetect::defaultsForEmulator(const QString& emulatorId)
{
    return DeviceProfile::defaultsFor(active(), emulatorId);
}

void DeviceProfileDetect::setOverride(const QString& kindToken)
{
    const QString t = kindToken.trimmed();
    if (t.isEmpty() || DeviceProfile::kindFromToken(t) == DeviceProfile::Kind::Unknown)
        store().remove(kOverrideKey);   // clear -> revert to detection (Unknown token is not a valid pin)
    else
        store().setValue(kOverrideKey, DeviceProfile::kindToken(DeviceProfile::kindFromToken(t)));  // canonicalise
    store().sync();
    invalidate();
}

QString DeviceProfileDetect::overrideToken()
{
    const QString ov = store().value(kOverrideKey).toString().trimmed();
    if (ov.isEmpty() || DeviceProfile::kindFromToken(ov) == DeviceProfile::Kind::Unknown) return QString();
    return DeviceProfile::kindToken(DeviceProfile::kindFromToken(ov));
}

void DeviceProfileDetect::invalidate()
{
    mMemoBuilt = false;
    mMemoDetected = DeviceProfile::Profile{};
}
