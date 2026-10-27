// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard Settings - a small Qt app to watch and configure the FocusGuard
// daemon. It edits the daemon's settings file and talks to the running
// daemon over its control channel; FocusGuard itself works without it.
#include <QApplication>
#include <QLocalServer>
#include <QLocalSocket>

#include "daemon_client.h"
#include "main_window.h"
#include "platform.h"
#include "settings_model.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("FocusGuard Settings");
    QApplication::setApplicationVersion(fg::kVersion);
    QApplication::setDesktopFileName("focusguard-settings");

    // One window per user: a second launch just raises the first.
    const QString instanceName = platform::controlEndpoint() + "-settings";
    {
        QLocalSocket other;
        other.connectToServer(instanceName);
        if (other.waitForConnected(300)) {
            other.write("show\n");
            other.waitForBytesWritten(300);
            return 0;
        }
    }
    QLocalServer instance;
    instance.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(instanceName);
    instance.listen(instanceName);

    DaemonClient client;
    SettingsModel model;
    MainWindow window(&client, &model);
    QObject::connect(&instance, &QLocalServer::newConnection, &window, [&] {
        while (QLocalSocket* s = instance.nextPendingConnection()) s->deleteLater();
        window.showNormal();
        window.raise();
        window.activateWindow();
    });
    window.show();
    return app.exec();
}
