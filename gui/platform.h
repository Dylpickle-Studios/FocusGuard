// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard Settings - the platform-specific bits: where things live, how to
// start the daemon, and starting it at login.
#pragma once

#include <QSettings>
#include <QString>

namespace platform {

QSettings& preferences();     // the app's own choices (not FocusGuard's settings file)
QString controlEndpoint();    // the daemon's socket / pipe name
QString defaultConfigPath();  // where the daemon looks when given no -c
QString daemonProgram();      // the FocusGuard executable to launch, or empty
bool startDaemon(QString* error);
bool autostartEnabled();
bool setAutostart(bool on, QString* error);

}  // namespace platform
