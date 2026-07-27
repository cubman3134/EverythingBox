// Edge-to-edge support: the window disables Qt's automatic safe-area contents margins (so the themed
// surface reaches the physical screen edges like a native media app), and the REAL platform insets are
// republished to the QML surfaces through this bridge so chrome (panel headers, list tails) still clears
// the notch / Dynamic Island / home indicator. Non-Apple platforms report zero insets.
#pragma once
#include <QObject>
#include <QMarginsF>

// Platform query: the key window's safe-area insets in points (iOS .mm implementation; zeros elsewhere).
QMarginsF mmvSafeAreaInsets();

// iOS: put the AVAudioSession in the Playback category. Qt's default session is 'ambient', which the
// RINGER/SILENT switch mutes — a media app's sounds and video audio must play regardless of the switch
// (every video app does this). No-op off iOS.
void mmvConfigureAudioSession();

// The `safeArea` context property: QML binds top/bottom; refresh() re-reads the platform (call it once the
// window is up — the insets are zero before the platform window exists — and on orientation changes).
class SafeAreaBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qreal top READ top NOTIFY changed)
    Q_PROPERTY(qreal bottom READ bottom NOTIFY changed)
    Q_PROPERTY(qreal left READ left NOTIFY changed)
    Q_PROPERTY(qreal right READ right NOTIFY changed)
public:
    static SafeAreaBridge& instance()
    {
        static SafeAreaBridge s;
        return s;
    }
    qreal top() const { return m_.top(); }
    qreal bottom() const { return m_.bottom(); }
    qreal left() const { return m_.left(); }
    qreal right() const { return m_.right(); }
public slots:
    void refresh()
    {
        const QMarginsF now = mmvSafeAreaInsets();
        if (now != m_) { m_ = now; emit changed(); }
    }
signals:
    void changed();
private:
    QMarginsF m_;
};
