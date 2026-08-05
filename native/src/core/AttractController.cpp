#include "AttractController.h"
#include "../addons/AddonModels.h"

// Best-first. See preferredRoles() in the header for why a portrait box/poster is intentionally excluded.
QStringList AttractController::preferredRoles()
{
    return { QStringLiteral("fanart"), QStringLiteral("hero"), QStringLiteral("background"),
             QStringLiteral("screenshot") };
}

AttractSlide AttractController::slideFor(const QString& title, const MediaArt& art)
{
    AttractSlide s;
    s.title = title;
    for (const QString& role : preferredRoles())
    {
        const QString url = art.image(role);   // first (best) candidate for the role, else ""
        if (!url.isEmpty()) { s.art = url; break; }
    }
    return s;   // invalid (art empty) when the game carries none of the preferred roles
}

QVector<AttractSlide> AttractController::buildSlides(const QVector<QPair<QString, MediaArt>>& library)
{
    QVector<AttractSlide> out;
    for (const auto& pr : library)
    {
        const AttractSlide s = slideFor(pr.first, pr.second);
        if (s.isValid()) out.push_back(s);   // a game with no usable art is skipped, never shown blank
    }
    return out;
}

void AttractController::setEnabled(bool on)
{
    enabled_ = on;
    if (!on && active_) active_ = false;   // turning the feature off must not leave a slideshow running
}

void AttractController::setTimeoutMs(qint64 ms)
{
    timeoutMs_ = ms > 0 ? ms : timeoutMs_;   // ignore a nonsensical non-positive timeout
}

void AttractController::setPlaybackActive(bool on)
{
    playbackActive_ = on;
    if (on && active_) active_ = false;      // content just came on screen: never keep the screensaver over it
}

void AttractController::setSlides(const QVector<AttractSlide>& slides)
{
    slides_ = slides;
    if (index_ >= slides_.size()) index_ = 0;
    if (slides_.isEmpty()) active_ = false;  // nothing to show any more
}

AttractController::InputResult AttractController::noteInput(qint64 nowMs)
{
    lastInputMs_ = nowMs;   // ANY input resets the idle clock, active or not
    InputResult r;
    if (active_)
    {
        // First press only wakes: dismiss AND swallow this one, restoring the captured view.
        active_ = false;
        r.swallow = true;
        r.dismissed = true;
        r.restoreToken = restoreToken_;
    }
    return r;               // not active: neither swallowed nor dismissed — navigation flows normally
}

bool AttractController::canEnter(qint64 nowMs) const
{
    if (!enabled_) return false;
    if (playbackActive_) return false;
    if (active_) return false;
    if (slides_.isEmpty()) return false;
    return (nowMs - lastInputMs_) >= timeoutMs_;
}

bool AttractController::poll(qint64 nowMs, const QString& viewToken, int startIndex)
{
    // While content is on screen the idle clock is held at "now", so the timeout is measured only from menu
    // time — the moment playback ends, a fresh full timeout must pass before attract can fire.
    if (playbackActive_) { lastInputMs_ = nowMs; return false; }
    if (!canEnter(nowMs)) return false;
    return enter(viewToken, startIndex);
}

bool AttractController::enter(const QString& viewToken, int startIndex)
{
    if (slides_.isEmpty() || active_) return false;
    active_ = true;
    restoreToken_ = viewToken;
    index_ = ((startIndex % slides_.size()) + slides_.size()) % slides_.size();   // wrap, tolerate negatives
    return true;
}

AttractSlide AttractController::currentSlide() const
{
    if (!active_ || slides_.isEmpty()) return AttractSlide{};
    return slides_.at(index_);
}

int AttractController::advance()
{
    if (!active_ || slides_.isEmpty()) return index_;
    index_ = (index_ + 1) % slides_.size();   // wrap at the end
    return index_;
}
