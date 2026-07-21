#include "FormFactor.h"
#include "../core/Settings.h"   // relative (matches ThemeEngine's ../core/… convention): resolves in the app build too

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

// Phase 1: desktop builds always resolve Desktop. Android branches land in Phase 2 (this is the single
// seam a platform-detection change touches).
FormFactor::Mode FormFactor::resolveAuto()
{
    return Mode::Desktop;
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
