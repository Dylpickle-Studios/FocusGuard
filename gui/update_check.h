// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard Settings - the optional check for a newer release. Off unless
// the user turns it on; then it asks GitHub for the latest release at most
// once a day. Nothing but that request is sent.
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;

class UpdateCheck : public QObject {
    Q_OBJECT
public:
    explicit UpdateCheck(QObject* parent = nullptr);

    bool enabled() const;
    void setEnabled(bool on);  // remembered; turning it on checks right away
    void checkIfDue();         // asks GitHub if the last answer is a day old

    // The newest release seen that is newer than this app, if any.
    QString newerVersion() const;
    QString releasePage() const;

signals:
    void changed();  // newerVersion() may have changed

private:
    void check();

    QNetworkAccessManager* net_ = nullptr;  // created on first use
    QTimer timer_;
    bool busy_ = false;
};
