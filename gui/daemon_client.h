// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard Settings - connection to the running daemon over its control
// channel (see src/control.hpp). Reconnects by itself whenever the daemon
// starts, restarts or goes away.
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>

class DaemonClient : public QObject {
    Q_OBJECT
public:
    explicit DaemonClient(QObject* parent = nullptr);
    ~DaemonClient() override;

    bool isConnected() const { return connected_; }
    const QJsonObject& status() const { return status_; }  // empty while disconnected
    void send(const QString& command);
    void reconnectSoon(int ms = 300);

signals:
    void connectionChanged(bool connected);
    void statusChanged(const QJsonObject& status);
    void eventReceived(const QJsonObject& event);
    void historyReceived(const QJsonArray& events);
    void windowsReceived(const QJsonArray& windows);

private:
    void tryConnect();
    void onReadyRead();
    void setConnected(bool on);

    QTimer retry_;  // declared before sock_ so it outlives it
    QLocalSocket sock_;
    QJsonObject status_;
    bool connected_ = false;
};
