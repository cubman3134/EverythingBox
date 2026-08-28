// PlayerIconProvider — the drawn transport glyphs (PlayerIcons), served to QML.
//
// The themed now-playing page spelled its transport the way the classic bar used to: as Unicode media
// characters. Which meant the same defect, in a worse place — a themed surface is the one place where a
// theme's own `foreground` decides what colour the chrome is, and a colour emoji font overrode it. The page
// asked for near-white and got saturated blue lozenges, whatever the theme said.
//
// So QML draws the same shapes the widget bar draws, from the same coordinates, through this provider:
//
//     Image { source: "image://ebicon/playPause/" + colourHex }   // colourHex without the leading '#'
//
// The colour rides in the URL rather than being applied as a QML layer effect because the software Qt Quick
// backend this app forces has no shader layers — a ColorOverlay would silently do nothing. It also makes the
// URL the whole cache key, so a theme change simply asks for a different image.
//
// Unknown names return a null pixmap rather than a placeholder: an icon that quietly turns into a question
// mark is how a typo ships. A blank button is noticed.
#pragma once
#include <QQuickImageProvider>
#include <QStringList>

#include "../ui/PlayerIcons.h"

class PlayerIconProvider : public QQuickImageProvider
{
public:
    PlayerIconProvider() : QQuickImageProvider(QQuickImageProvider::Pixmap) {}

    QPixmap requestPixmap(const QString& id, QSize* size, const QSize& requestedSize) override
    {
        // "<name>" or "<name>/<rrggbb>" — the id is everything after image://ebicon/.
        const QStringList parts = id.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) return {};

        bool known = false;
        const PlayerIcons::Glyph g = glyphFor(parts.first(), &known);
        if (!known) return {};

        QColor c = PlayerIcons::defaultColor();
        if (parts.size() > 1)
        {
            const QColor asked(QLatin1Char('#') + parts.at(1));
            if (asked.isValid()) c = asked;
        }

        // A themed page sizes its buttons off the window, so the request can be for anything; the fallback is
        // the size the widget bar draws at. Painted at the requested size rather than scaled up from a smaller
        // one — the whole point of geometry is that it is sharp at whatever size it lands on.
        const int px = requestedSize.width() > 0  ? requestedSize.width()
                     : requestedSize.height() > 0 ? requestedSize.height()
                                                  : PlayerIcons::kTransportPx;
        // Device pixels: QQuickImageProvider's requestedSize is already in device pixels for a scaled screen,
        // so this must NOT multiply by the ratio again.
        QPixmap pm = PlayerIcons::pixmap(g, px, c, 1.0);
        if (size) *size = pm.size();
        return pm;
    }

private:
    // The names QML uses. Deliberately the page's own transport verbs where they line up (playPause, seekBack,
    // seekFwd, stop), so a delegate can hand its verb straight to the URL, plus the plain names for the rest.
    static PlayerIcons::Glyph glyphFor(const QString& name, bool* known)
    {
        *known = true;
        if (name == QLatin1String("prevTrack") || name == QLatin1String("prev"))  return PlayerIcons::PrevChapter;
        if (name == QLatin1String("nextTrack") || name == QLatin1String("next"))  return PlayerIcons::NextChapter;
        if (name == QLatin1String("seekBack")  || name == QLatin1String("rewind"))return PlayerIcons::Rewind;
        if (name == QLatin1String("seekFwd") || name == QLatin1String("fastForward")) return PlayerIcons::FastForward;
        if (name == QLatin1String("playPause"))  return PlayerIcons::PlayPause;
        if (name == QLatin1String("play"))       return PlayerIcons::Play;
        if (name == QLatin1String("pause"))      return PlayerIcons::Pause;
        if (name == QLatin1String("stop"))       return PlayerIcons::Stop;
        if (name == QLatin1String("volume"))     return PlayerIcons::Volume;
        if (name == QLatin1String("volumeMuted"))return PlayerIcons::VolumeMuted;
        if (name == QLatin1String("volumeBoost"))return PlayerIcons::VolumeBoost;
        if (name == QLatin1String("gear"))       return PlayerIcons::Gear;
        if (name == QLatin1String("warning"))    return PlayerIcons::Warning;
        *known = false;
        return PlayerIcons::Play;
    }
};
