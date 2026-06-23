#include <QApplication>
#include <QIcon>
#include <clocale>
#include "ui/MainWindow.h"
#include "ui/ProfileDialog.h"
#include "core/ProfileStore.h"

int main(int argc, char** argv)
{
    // libmpv requires the C numeric locale, otherwise option/number parsing breaks. Set it before Qt.
    std::setlocale(LC_NUMERIC, "C");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("My Media Vault"));
    QApplication::setApplicationDisplayName(QStringLiteral("My Media Vault"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/appicon.png")));

    // A profile must be active before the app opens. One profile -> use it; otherwise (none, or several)
    // the user picks an existing profile or creates one. Refusing to choose at startup exits the app.
    const QVector<Profile> profiles = ProfileStore::list();
    if (profiles.size() == 1)
    {
        ProfileStore::setCurrent(profiles.first().id);
    }
    else
    {
        ProfileDialog dlg(/*mustChoose*/ true);
        if (dlg.exec() != QDialog::Accepted || dlg.selectedId().isEmpty())
            return 0; // no profile chosen -> don't start
        ProfileStore::setCurrent(dlg.selectedId());
    }

    MainWindow window;
    window.setWindowTitle(QStringLiteral("My Media Vault"));
    window.resize(1280, 760);
    window.show();
    window.raise();
    window.activateWindow(); // foreground + keyboard focus so arrow keys work without a click first
    return app.exec();
}
