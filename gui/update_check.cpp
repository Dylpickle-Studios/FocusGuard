// SPDX-License-Identifier: AGPL-3.0-or-later
#include "update_check.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "common.hpp"
#include "platform.h"

namespace {

constexpr const char* kLatestRelease = "https://api.github.com/repos/DylanBeMe/FocusGuard/releases/latest";
constexpr qint64 kDayMs = 24LL * 60 * 60 * 1000;

}  // namespace

UpdateCheck::UpdateCheck(QObject* parent) : QObject(parent) {
    // An app left open for days still checks daily.
    timer_.setInterval(6 * 60 * 60 * 1000);
    connect(&timer_, &QTimer::timeout, this, &UpdateCheck::checkIfDue);
    if (enabled()) timer_.start();
}

bool UpdateCheck::enabled() const { return platform::preferences().value("updates/check", false).toBool(); }

void UpdateCheck::setEnabled(bool on) {
    platform::preferences().setValue("updates/check", on);
    if (on) {
        timer_.start();
        check();
    } else {
        timer_.stop();
        emit changed();  // hide what an earlier check found
    }
}

void UpdateCheck::checkIfDue() {
    if (!enabled()) return;
    const qint64 last = platform::preferences().value("updates/lastCheck", 0).toLongLong();
    if (QDateTime::currentMSecsSinceEpoch() - last >= kDayMs) check();
}

QString UpdateCheck::newerVersion() const {
    if (!enabled()) return {};
    const QString v = platform::preferences().value("updates/latest").toString();
    return fg::version_newer(v.toStdString(), fg::kVersion) ? v : QString();
}

QString UpdateCheck::releasePage() const {
    return platform::preferences().value("updates/page", "https://github.com/DylanBeMe/FocusGuard/releases/latest")
        .toString();
}

void UpdateCheck::check() {
    if (busy_) return;
    busy_ = true;
    if (!net_) net_ = new QNetworkAccessManager(this);
    QNetworkRequest req{QUrl(kLatestRelease)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("FocusGuard-Settings/%1").arg(fg::kVersion));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setTransferTimeout(20000);
    QNetworkReply* reply = net_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        busy_ = false;
        if (reply->error() != QNetworkReply::NoError) return;  // offline: try again next time
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = o.value("tag_name").toString();
        if (tag.isEmpty()) return;
        QSettings& prefs = platform::preferences();
        prefs.setValue("updates/lastCheck", QDateTime::currentMSecsSinceEpoch());
        prefs.setValue("updates/latest", tag.startsWith('v') ? tag.mid(1) : tag);
        const QString page = o.value("html_url").toString();
        if (page.startsWith("https://github.com/")) prefs.setValue("updates/page", page);
        emit changed();
    });
}
