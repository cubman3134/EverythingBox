#include "ThemeFormFactors.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonValue>

namespace ThemeFormFactors
{

Fit fit(const QJsonValue& declared, const QString& currentMode)
{
    // NOT an array -> no declaration. Absent, null, a bare "desktop", an object, a number: all Undeclared.
    // See the header for why this is strict rather than forgiving.
    if (!declared.isArray()) return Fit::Undeclared;

    const QString want = currentMode.trimmed().toLower();
    const QJsonArray arr = declared.toArray();
    for (const QJsonValue& v : arr)
    {
        // toString() yields an empty string for a non-string entry, so numbers/objects/nulls fall out here
        // rather than matching an empty currentMode by accident.
        const QString label = v.toString().trimmed().toLower();
        // A blank entry is not a label. This ONLY changes the answer when `currentMode` is itself empty — a
        // caller that never resolved a mode — and it makes that case read Unsupported (loud: every theme is
        // flagged) rather than Supported (silent: the feature quietly does nothing). Deliberate tripwire.
        if (label.isEmpty()) continue;
        if (label == want) return Fit::Supported;
    }
    // Includes the empty array: declaring [] is a real claim ("fits nothing"), not a missing declaration.
    return Fit::Unsupported;
}

QString shortNote(Fit f)
{
    // The ONE wording, shared by all three surfaces that show it — the themed picker rows, the themed
    // Appearance "Theme…" row, and the classic Appearance theme list. Kept here rather than at each surface
    // because three hand-written copies of a user-facing sentence drift, and the picker's copy is the one
    // nobody re-reads. Translatable: QCoreApplication::translate works in a QtCore-only unit, and the QML
    // renders the string this returns rather than one of its own.
    switch (f)
    {
        case Fit::Supported:
            return QString();   // nothing to say; a fitting theme gets no decoration at all
        case Fit::Unsupported:
            // The author listed devices and did not list this one. Says what is KNOWN, not what will happen:
            // the claim is unverified either way, so promising "this will look wrong" would overstate it.
            return QCoreApplication::translate("ThemeFormFactors", "Not listed for this device");
        case Fit::Undeclared:
            return QCoreApplication::translate("ThemeFormFactors", "Device support not declared");
    }
    return QString();
}

} // namespace ThemeFormFactors
