// A user-selectable colour theme, à la EmulationStation/RetroBat theme sets (but a single JSON file instead
// of an XML layout engine). Built-in themes plus any "*.json" the user drops into <app>/themes/. The chosen
// theme is per profile. A theme controls the accent behaviour and the media-tab colours; the app stays light.
#pragma once
#include <QString>
#include <QVector>
#include <QHash>
#include <QColor>

// Declarative detail-page layout (an ES-style "view"): where the cover sits and the order of the text rows.
struct ThemeDetail
{
    QString image = QStringLiteral("left"); // "left" | "top" | "hidden"
    int imageWidth = 170;
    QStringList order;                       // section order; empty -> {favorite,title,facts,overview}
};

struct Theme
{
    QString name;
    bool accentFollowsTab = true;             // accent = selected tab's colour; else the fixed `accent`
    QColor accent = QColor(0x53, 0x82, 0xC4); // used when accentFollowsTab is false
    QColor neutralTab = QColor(0x7E, 0x82, 0x8C);
    QHash<QString, QColor> tabColors;         // media type -> colour override (e.g. "movie" -> "#...")

    // ES-style additions:
    QString fontFamily;                       // "" = app default
    double  fontScale = 1.0;                  // scales the whole UI font
    int     cornerRadius = 9;                 // search box / cards
    QString background;                       // resolved background-image path ("" = themed tint only)
    double  backgroundDim = 0.55;             // overlay opacity over the background (readability)
    QHash<QString, QString> icons;            // media type -> resolved placeholder-icon image path
    ThemeDetail detail;                       // declarative detail-page layout
    QString layout = QStringLiteral("tabs");  // media-type layer: "tabs" (default) | "carousel"
};

namespace ThemeStore
{
    QVector<Theme> all();               // built-in themes + user themes from <app>/themes/*.json
    Theme byName(const QString& name);  // resolve by name; falls back to the default theme
    QString currentName();              // the active profile's chosen theme
    void setCurrent(const QString& name);
    Theme current();
}
