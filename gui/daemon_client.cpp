// SPDX-License-Identifier: AGPL-3.0-or-later
#include "daemon_client.h"

#include <QJsonDocument>

#include "platform.h"

DaemonClient::DaemonClient(QObject* parent) : QObject(parent) {
    retry_.setSingleShot(true);
    retry_.setInterval(1500);
    connect(&retry_, &QTimer::timeout, this, &DaemonClient::tryConnect);
    connect(&sock_, &QLocalSocket::connected, this, [this] {
        setConnected(true);
        send("status");
        send("events");
        send("subscribe");
    });
    connect(&sock_, &QLocalSocket::disconnected, this, [this] {
        setConnected(false);
        retry_.start();
    });
    connect(&sock_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        if (sock_.state() != QLocalSocket::ConnectedState) {
            setConnected(false);
            retry_.start();
        }
    });
    connect(&sock_, &QLocalSocket::readyRead, this, &DaemonClient::onReadyRead);
    tryConnect();
}

DaemonClient::~DaemonClient() {
    // The socket says "disconnected" while it is destroyed; don't react to that.
    disconnect(&sock_, nullptr, this, nullptr);
}

void DaemonClient::tryConnect() {
    if (sock_.state() != QLocalSocket::UnconnectedState) return;
    sock_.connectToServer(platform::controlEndpoint());
}

void DaemonClient::reconnectSoon(int ms) {
    retry_.start(ms);
}

void DaemonClient::send(const QString& command) {
    if (!connected_) return;
    sock_.write(command.toUtf8() + '\n');
}

void DaemonClient::setConnected(bool on) {
    if (on == connected_) return;
    connected_ = on;
    if (!on) {
        status_ = {};
        emit statusChanged(status_);
    }
    emit connectionChanged(on);
}

void DaemonClient::onReadyRead() {
    while (sock_.canReadLine()) {
        const QByteArray line = sock_.readLine();
        const QJsonObject msg = QJsonDocument::fromJson(line).object();
        const QString type = msg.value("type").toString();
        if (type == "status") {
            status_ = msg;
            emit statusChanged(status_);
        } else if (type == "event") {
            emit eventReceived(msg);
        } else if (type == "events") {
            emit historyReceived(msg.value("items").toArray());
        } else if (type == "windows") {
            emit windowsReceived(msg.value("items").toArray());
        }
    }
}
