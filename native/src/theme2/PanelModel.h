// PanelModel — the descriptor for one row of a themed settings panel (ThemedPanelHost / SettingsPanel.qml),
// plus the C++→QML marshaling. The classic showPanel builders (a lambda that stuffs QWidgets into a layout)
// become, in themed mode, a flat QVector<PanelRow>: pure data the host renders through the Nav Contract. Six
// later B2 conversions (General, Appearance, Downloads, Cloud Sync, Debug, …) consume this struct verbatim, so
// its shape is frozen here.
#pragma once
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

// One row of a themed settings panel. The classic showPanel builders become lists of these.
struct PanelRow {
    enum Kind { Action, Toggle, Choice, TextField, Info, Progress, Separator, LogView };
    // LogView: scrollable read-only text (Debug log) — activate = scroll mode, Esc = back
    // (mirrors NavTextField's read-only two-state semantics). Rendered only when used (Task 3).
    Kind kind = Action;
    QString id;            // stable row id — activation dispatches on it
    QString label;         // left text
    QString value;         // right text (Info), current text (TextField), current option (Choice)
    QStringList options;   // Choice options
    bool checked = false;  // Toggle state
    int  progress = -1;    // 0..100 for Progress rows
    bool enabled = true;
    bool destructive = false; // styled with the warning accent (Uninstall)
    bool masked = false;      // TextField: render the value as dots (credentials) — the OSK editor is unchanged

    // An optional picture for the row, as a LOCAL FILE PATH — the theme gallery's screenshot thumbnails
    // (issue #91). Never a remote url: the QML would then do its own unbounded, unjudged download, and the
    // whole of ThemeShots (the size cap, the magic-byte rule, the cache) exists so that it does not. Empty
    // for every row that has no picture, which is every row this product had before, and an empty value
    // renders nothing and occupies no width — the row is laid out exactly as it was.
    QString thumbnail;

    // The single-row map SettingsPanel.qml's ListView delegate binds. Kind is marshaled as an int the QML
    // switches on (see SettingsPanel.qml's kind* readonly ints); options become a JS array of strings.
    QVariantMap toMap() const
    {
        QVariantMap m;
        m.insert(QStringLiteral("kind"), int(kind));
        m.insert(QStringLiteral("id"), id);
        m.insert(QStringLiteral("label"), label);
        m.insert(QStringLiteral("value"), value);
        m.insert(QStringLiteral("options"), QVariant(options));
        m.insert(QStringLiteral("checked"), checked);
        m.insert(QStringLiteral("progress"), progress);
        m.insert(QStringLiteral("enabled"), enabled);
        m.insert(QStringLiteral("destructive"), destructive);
        m.insert(QStringLiteral("masked"), masked);
        // Marshaled as a file: URL, not as the path. A bare path assigned to an Image's `source` is resolved
        // against the QML file's own base url — which turns an absolute Windows path into a nonsense url and
        // a POSIX one into a file with too many slashes — and getting that wrong is a picture that silently
        // never appears. C++ holds the path (that is what every other caller deals in); the ONE conversion
        // lives here, where kind is already converted to an int for the same reason.
        m.insert(QStringLiteral("thumbnail"),
                 thumbnail.isEmpty() ? QString() : QUrl::fromLocalFile(thumbnail).toString());
        return m;
    }
};

// Marshal a row list into the model SettingsPanel.qml's ListView consumes (a QVariantList of row maps).
inline QVariantList panelRowsToVariant(const QVector<PanelRow>& rows)
{
    QVariantList out;
    out.reserve(rows.size());
    for (const PanelRow& r : rows) out << r.toMap();
    return out;
}
