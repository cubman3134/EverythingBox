#include <QApplication>
#include <clocale>
#include "ui/MainWindow.h"

int main(int argc, char** argv)
{
    // libmpv requires the C numeric locale, otherwise option/number parsing breaks. Set it before Qt.
    std::setlocale(LC_NUMERIC, "C");

    QApplication app(argc, argv);
    MainWindow window;
    window.setWindowTitle(QStringLiteral("Project Goliath"));
    window.resize(1280, 760);
    window.show();
    return app.exec();
}
