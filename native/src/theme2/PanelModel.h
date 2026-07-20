// PanelModel — the descriptor for one row of a themed settings panel (ThemedPanelHost / SettingsPanel.qml),
// plus the C++→QML marshaling. The classic showPanel builders (a lambda that stuffs QWidgets into a layout)
// become, in themed mode, a flat QVector<PanelRow>: pure data the host renders through the Nav Contract. Six
// later B2 conversions (General, Appearance, Downloads, Cloud Sync, Debug, …) consume this struct verbatim, so
// its shape is frozen here.
#pragma once
#include <QString>
#include <QStringList>
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
