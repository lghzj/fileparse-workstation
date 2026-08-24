#include <QApplication>

#include "ui/MainWindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("NetStar Workstation");
    QApplication::setApplicationVersion(APP_VERSION);
    QApplication::setOrganizationName("NetStar");

    MainWindow window;
    window.resize(1360, 820);
    window.show();

    return QApplication::exec();
}
