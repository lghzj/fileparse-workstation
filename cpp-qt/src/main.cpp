#include <QApplication>
#include <QLocalServer>
#include <QLocalSocket>

#include "ui/MainWindow.h"

namespace {
constexpr const char *kSingleInstanceServerName = "netstar-parse-hub-workstation";

bool notifyExistingInstance() {
    QLocalSocket socket;
    socket.connectToServer(kSingleInstanceServerName);
    if (!socket.waitForConnected(200)) {
        return false;
    }
    socket.write("activate");
    socket.flush();
    socket.waitForBytesWritten(200);
    return true;
}
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("NetStar Workstation");
    QApplication::setApplicationVersion(APP_VERSION);
    QApplication::setOrganizationName("NetStar");

    if (notifyExistingInstance()) {
        return 0;
    }

    QLocalServer::removeServer(kSingleInstanceServerName);
    QLocalServer singleInstanceServer;
    singleInstanceServer.listen(kSingleInstanceServerName);

    MainWindow window;
    window.resize(1360, 820);
    window.show();

    QObject::connect(&singleInstanceServer, &QLocalServer::newConnection, &window, [&]() {
        while (QLocalSocket *client = singleInstanceServer.nextPendingConnection()) {
            client->deleteLater();
        }
        window.showNormal();
        window.raise();
        window.activateWindow();
    });

    return QApplication::exec();
}
