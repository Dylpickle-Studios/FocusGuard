// SPDX-License-Identifier: AGPL-3.0-or-later
#include "platform.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "control.hpp"

namespace platform {

QSettings& preferences() {
    static QSettings prefs(QSettings::NativeFormat, QSettings::UserScope, "FocusGuard", "FocusGuard Settings");
    return prefs;
}

QString controlEndpoint() {
#ifdef Q_OS_WIN
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return QString::fromStdString(fg::control_endpoint(session));
#else
    return QString::fromStdString(fg::control_endpoint());
#endif
}

QString defaultConfigPath() {
#ifdef Q_OS_WIN
    return QDir::toNativeSeparators(qEnvironmentVariable("APPDATA") + "/FocusGuard/focusguard.conf");
#else
    const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    return (xdg.isEmpty() ? QDir::homePath() + "/.config" : xdg) + "/focusguard.conf";
#endif
}

QString daemonProgram() {
    const QDir here(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    for (const char* name : {"focusguard-tray.exe", "focusguard.exe"})  // prefer the build without a console
        if (here.exists(name)) return QDir::toNativeSeparators(here.filePath(name));
    return {};
#else
    if (QFileInfo(here.filePath("focusguard")).isExecutable()) return here.filePath("focusguard");
    return QStandardPaths::findExecutable("focusguard");
#endif
}

#ifdef Q_OS_WIN
static bool isConsoleBuild(const QString& program) {
    return QFileInfo(program).fileName().compare("focusguard.exe", Qt::CaseInsensitive) == 0;
}
#endif

bool startDaemon(QString* error) {
    const QString program = daemonProgram();
    if (program.isEmpty()) {
        if (error) *error = QCoreApplication::translate("platform", "FocusGuard wasn't found next to this app or on the PATH.");
        return false;
    }
    QProcess proc;
    proc.setProgram(program);
#ifdef Q_OS_WIN
    if (isConsoleBuild(program)) {
        proc.setArguments({"--background"});
        proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
            args->flags |= CREATE_NO_WINDOW;
        });
    }
#endif
    proc.setStandardOutputFile(QProcess::nullDevice());
    proc.setStandardErrorFile(QProcess::nullDevice());
    if (!proc.startDetached()) {
        if (error) *error = QCoreApplication::translate("platform", "Couldn't start %1.").arg(program);
        return false;
    }
    return true;
}

#ifdef Q_OS_WIN
static const char* kRunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool autostartEnabled() {
    return QSettings(kRunKey, QSettings::NativeFormat).contains("FocusGuard");
}

bool setAutostart(bool on, QString* error) {
    QSettings run(kRunKey, QSettings::NativeFormat);
    if (!on) {
        run.remove("FocusGuard");
    } else {
        const QString program = daemonProgram();
        if (program.isEmpty()) {
            if (error) *error = QCoreApplication::translate("platform", "FocusGuard wasn't found next to this app.");
            return false;
        }
        run.setValue("FocusGuard", "\"" + program + "\"" + (isConsoleBuild(program) ? " --background" : ""));
    }
    run.sync();
    if (run.status() != QSettings::NoError) {
        if (error) *error = QCoreApplication::translate("platform", "Couldn't change the startup entry in the registry.");
        return false;
    }
    return true;
}
#else
static QString autostartFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/autostart/focusguard.desktop";
}

bool autostartEnabled() {
    QFile f(autostartFile());
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray text = f.readAll();
    return !text.contains("Hidden=true") && !text.contains("X-GNOME-Autostart-enabled=false");
}

bool setAutostart(bool on, QString* error) {
    const QString path = autostartFile();
    if (!on) {
        if (QFile::exists(path) && !QFile::remove(path)) {
            if (error) *error = QCoreApplication::translate("platform", "Couldn't remove %1.").arg(path);
            return false;
        }
        return true;
    }
    QString program = daemonProgram();
    if (program.isEmpty()) {
        if (error) *error = QCoreApplication::translate("platform", "FocusGuard wasn't found next to this app or on the PATH.");
        return false;
    }
    if (program.contains(' ')) program = "\"" + program + "\"";
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = QCoreApplication::translate("platform", "Couldn't write %1.").arg(path);
        return false;
    }
    f.write("[Desktop Entry]\n"
            "Type=Application\n"
            "Name=FocusGuard\n"
            "Comment=Stops windows from stealing keyboard focus\n"
            "Exec=" + program.toUtf8() + "\n"
            "Icon=focusguard\n"
            "NoDisplay=true\n"
            "X-GNOME-Autostart-enabled=true\n");
    if (!f.commit()) {
        if (error) *error = QCoreApplication::translate("platform", "Couldn't write %1.").arg(path);
        return false;
    }
    return true;
}
#endif

}  // namespace platform
