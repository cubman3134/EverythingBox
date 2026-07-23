#include "FormFactor.h"
#include "../core/Settings.h"   // relative (matches ThemeEngine's ../core/… convention): resolves in the app build too
#ifdef Q_OS_ANDROID
#include <QCoreApplication>     // QNativeInterface::QAndroidApplication::context()
#include <QJniObject>           // JNI call into Android's UiModeManager for leanback (TV) detection
#endif

// Mode -> adaptivity tokens (spec: subsystem D §1). The Desktop row is IDENTITY by contract — uiScale 1.0,
// minHitPx 0, safeAreaFrac 0.0, density 1.0 — so every consumer that multiplies/insets by these is a
// pixel-for-pixel no-op in Desktop mode. All four accessors switch on mode_ here, in ONE place, so a later
// task tunes the table without touching any consumer.

FormFactor::FormFactor()
{
    mode_ = resolveAuto();
    refresh(); // pick up any stored "display/mode" override at construction
}

FormFactor& FormFactor::instance()
{
    static FormFactor s; // function-local static: constructed once, on first use
    return s;
}

// Auto resolution (the single platform-detection seam):
//   Desktop builds  -> always Desktop (identity tokens; probe_formfactor pins this).
//   Android builds  -> ask Android's UiModeManager: UI_MODE_TYPE_TELEVISION (leanback) => Tv,
//                      everything else (touchscreen phones/tablets) => Mobile.
// QGuiApplication screen metrics / platform capabilities are NOT a reliable leanback signal, so we query
// the platform service directly over JNI. If the context/service isn't reachable we fall back to Mobile —
// the safe default on the only non-TV Android form factor (a phone/tablet).
FormFactor::Mode FormFactor::resolveAuto()
{
#ifdef Q_OS_ANDROID
    // Configuration.UI_MODE_TYPE_TELEVISION == 4 (stable Android API constant).
    constexpr jint UI_MODE_TYPE_TELEVISION = 4;
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (context.isValid())
    {
        QJniObject svcName = QJniObject::getStaticObjectField<jstring>(
            "android/content/Context", "UI_MODE_SERVICE");
        if (svcName.isValid())
        {
            QJniObject uiModeManager = context.callObjectMethod(
                "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", svcName.object<jstring>());
            if (uiModeManager.isValid())
            {
                const jint modeType = uiModeManager.callMethod<jint>("getCurrentModeType");
                if (modeType == UI_MODE_TYPE_TELEVISION)
                    return Mode::Tv;
            }
        }
    }
    return Mode::Mobile; // touchscreen phone/tablet (or context not yet reachable) -> Mobile
#elif defined(Q_OS_IOS)
    return Mode::Mobile; // iPhone/iPad are touch-first, same as Android phones/tablets
#else
    return Mode::Desktop;
#endif
}

void FormFactor::refresh()
{
    const QString stored = Settings::displayMode();
    Mode next;
    if (stored == QStringLiteral("desktop"))      next = Mode::Desktop;
    else if (stored == QStringLiteral("tv"))      next = Mode::Tv;
    else if (stored == QStringLiteral("mobile"))  next = Mode::Mobile;
    else                                          next = resolveAuto(); // "auto" and any unknown/corrupt string

    if (next == mode_) return; // no real change: do not emit
    mode_ = next;
    emit changed();
}

QString FormFactor::modeName() const
{
    switch (mode_)
    {
    case Mode::Tv:     return QStringLiteral("tv");
    case Mode::Mobile: return QStringLiteral("mobile");
    case Mode::Desktop: break;
    }
    return QStringLiteral("desktop");
}

qreal FormFactor::uiScale() const
{
    switch (mode_)
    {
    case Mode::Tv:     return 1.3;
    case Mode::Mobile: return 1.15;
    case Mode::Desktop: break;
    }
    return 1.0;
}

int FormFactor::minHitPx() const
{
    switch (mode_)
    {
    case Mode::Tv:     return 0;
    case Mode::Mobile: return 44;
    case Mode::Desktop: break;
    }
    return 0;
}

qreal FormFactor::safeAreaFrac() const
{
    switch (mode_)
    {
    case Mode::Tv:     return 0.05;
    case Mode::Mobile: return 0.0;
    case Mode::Desktop: break;
    }
    return 0.0;
}

qreal FormFactor::density() const
{
    switch (mode_)
    {
    case Mode::Tv:     return 1.15;
    case Mode::Mobile: return 1.1;
    case Mode::Desktop: break;
    }
    return 1.0;
}
