#pragma once
#include <QObject>

// The one place the C++ side and the themed QML agree about video hover previews (issue #55). It is a
// QtCore-only singleton exposed to the theme QML as the `videoPreview` context property (registered in
// ThemeEngine, next to `form`/`safeArea`), and it is ALSO the host the video snap player reports its
// audible-playing state to — so the two responsibilities of the feature meet here:
//
//   1. Settings -> QML. `enabled` and `volume` mirror Settings::videoPreviewsEnabled()/videoSnapVolume().
//      MainWindow pushes the stored values in (setEnabled/setVolume) at startup and on every settings change;
//      the bridge emits changed() so Video.qml re-reads them live (matching how `form`/`safeArea` update).
//      The bridge deliberately does NOT read Settings itself — keeping it a plain QObject with no core
//      dependency lets the MpvPreview probes link it without dragging Settings.cpp in.
//
//   2. Player -> BackgroundMusic duck. MpvPreview calls reportAudible(true) once it is actually painting
//      frames AND its volume is > 0, reportAudible(false) when it stops/clears/destructs. The count is
//      reference-counted (two snaps can briefly overlap on a fast hover hand-off), and duckRequested(bool)
//      fires on the 0<->1 edges. MainWindow connects it to BackgroundMusic::setDucked. At the muted default
//      (volume 0) no snap is ever "audible", so nothing ducks — exactly the "don't fight silence" rule.
class VideoPreviewBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(int volume READ volume NOTIFY changed)   // 0..100; 0 == muted
public:
    static VideoPreviewBridge& instance();

    bool enabled() const { return enabled_; }
    int  volume() const { return volume_; }

    void setEnabled(bool on);   // app pushes Settings::videoPreviewsEnabled(); emits changed() when it moves
    void setVolume(int pct);    // app pushes Settings::videoSnapVolume() (clamped 0..100); emits changed()

    // Called by the snap player: true while an AUDIBLE preview (playing && volume>0) is on screen, false when
    // it stops. Reference-counted; duckRequested fires only on the 0<->1 transitions.
    void reportAudible(bool on);

signals:
    void changed();               // enabled/volume changed -> QML re-reads the two properties
    void duckRequested(bool on);  // true when >=1 audible snap is playing; MainWindow ducks the BGM

private:
    VideoPreviewBridge() = default;
    bool enabled_ = true;
    int  volume_ = 0;
    int  audibleCount_ = 0;
};
