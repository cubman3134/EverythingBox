// MainWindow's FILE-SCOPE helpers that more than one MainWindow translation unit needs (issue #186).
//
// MainWindow.cpp is being split into focused translation units behind the same class. A member function
// that moves out takes with it every file-static only IT uses. A file-static the rest of MainWindow.cpp
// ALSO uses must not be copied: two copies drift apart, and a merge that edits one of them compiles on one
// side and not the other. So each such helper is defined ONCE, here, and every MainWindow TU that needs it
// includes this header. Internal to src/ui/MainWindow*.cpp; nothing else should include it.
//
// These were `static` in MainWindow.cpp. In a header they are `inline`, so there is still exactly ONE
// function -- and ONE QSettings object behind store() -- however many TUs include this. `inline` means
// external linkage, though, and ~25 other files under src/ define their own `QSettings& store()`: almost all
// in anonymous namespaces, but a global one with the same signature would be silently merged with this one
// by the linker. The named namespace keeps the symbol unique; the using-declarations below keep every call
// site spelled exactly as it was.
#pragma once

#include <QPushButton>
#include <QSettings>
#include <QString>

#include "../core/AppBrand.h"
#include "../core/AppPaths.h"

namespace mainwindow_internal {

// Per-profile settings store (resume positions, etc.), mirroring the accessor the other views use.
inline QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// A large, left-aligned menu row for the inline settings pages (TV/remote-friendly target size).
inline QPushButton* panelRow(const QString& label)
{
    auto* b = new QPushButton(label);
    b->setMinimumHeight(54);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral(
        "QPushButton{font-size:17px;text-align:left;padding:14px 20px;border:1px solid rgba(0,0,0,0.12);"
        "border-radius:10px;background:rgba(0,0,0,0.04);} QPushButton:hover{background:rgba(0,0,0,0.10);}"
        "QPushButton:focus{border:2px solid #2C72C9;}"));
    return b;
}

} // namespace mainwindow_internal

using mainwindow_internal::store;
using mainwindow_internal::panelRow;
