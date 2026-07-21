#pragma once
#include <QObject>

// The ONE form-factor authority (spec: subsystem D §1). Mode resolves from the stored
// "display/mode" setting ("auto"|"desktop"|"tv"|"mobile"); auto => platform detection
// (desktop builds resolve Desktop in Phase 1; Android branches land in Phase 2).
// Tokens derive from mode via the spec table. Desktop tokens are IDENTITY by contract:
// every consumer multiplied/inset by these must be a pixel-for-pixel no-op in Desktop.
class FormFactor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString mode READ modeName NOTIFY changed)
    Q_PROPERTY(qreal uiScale READ uiScale NOTIFY changed)
    Q_PROPERTY(int minHitPx READ minHitPx NOTIFY changed)
    Q_PROPERTY(qreal safeAreaFrac READ safeAreaFrac NOTIFY changed)
    Q_PROPERTY(qreal density READ density NOTIFY changed)
public:
    enum class Mode { Desktop, Tv, Mobile };
    static FormFactor& instance();

    Mode    mode() const { return mode_; }
    QString modeName() const;      // "desktop" | "tv" | "mobile"
    qreal   uiScale() const;       // table
    int     minHitPx() const;      // table (logical px; consumers already ride Qt DPR)
    qreal   safeAreaFrac() const;  // table
    qreal   density() const;       // table

    // The ONE size-derivation helper every widget-side consumer routes through: a base pixel size scaled by
    // uiScale() then floored to the minimum hit target. Desktop is identity (uiScale 1.0, minHitPx 0), so
    // hitClamp(n) == n on desktop — pixel-for-pixel no-op. applyFormFactorWidgets() pins the real path through
    // here; probe_formfactor asserts the table (hitClamp(46): desktop 46, tv 59, mobile 52).
    int     hitClamp(int basePx) const { return qMax(int(basePx * uiScale()), minHitPx()); }

    void    refresh();             // re-read Settings::displayMode(), re-resolve, emit changed() if different
    static Mode resolveAuto();     // Phase 1: always Desktop on desktop platforms
signals:
    void changed();
private:
    FormFactor();
    Mode mode_ = Mode::Desktop;
};
