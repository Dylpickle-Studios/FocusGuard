// SPDX-License-Identifier: AGPL-3.0-or-later
#include "settings_model.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>

SettingsModel::SettingsModel(QObject* parent) : QObject(parent) {
    auto onChange = [this] {
        watch();  // editors often replace the file, which drops the watch
        if (!dirty_) load();
    };
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, onChange);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, onChange);
}

void SettingsModel::setPath(const QString& path) {
    if (path == path_ || dirty_) return;
    path_ = path;
    load();
    watch();
}

void SettingsModel::watch() {
    if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
    if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
    if (path_.isEmpty()) return;
    const QFileInfo fi(path_);
    if (fi.exists()) watcher_.addPath(path_);
    if (fi.dir().exists()) watcher_.addPath(fi.absolutePath());  // notices the file being created
}

void SettingsModel::load() {
    fg::Config c;
    loadError_.clear();
    text_.clear();
    QFile f(path_);
    if (f.open(QIODevice::ReadOnly)) {
        text_ = f.readAll().toStdString();
        std::string err;
        if (fg::load_config_file(path_.toStdString(), c, err) == 1)
            loadError_ = QString::fromStdString(err);  // saving would lose what follows the bad line
    }
    config = c;
    saved_ = c;
    setDirty(false);
    emit refreshNeeded();
}

void SettingsModel::changed() {
    setDirty(fg::write_config(text_, config) != fg::write_config(text_, saved_));
}

void SettingsModel::setDirty(bool d) {
    if (d == dirty_) return;
    dirty_ = d;
    emit dirtyChanged(d);
}

bool SettingsModel::save(QString* error) {
    if (!canSave()) {
        if (error) *error = tr("The settings file has an error, so it can't be saved from here: %1").arg(loadError_);
        return false;
    }
    const std::string text = fg::write_config(text_, config);
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile f(path_);
    if (!f.open(QIODevice::WriteOnly) || f.write(text.data(), (qint64)text.size()) != (qint64)text.size() ||
        !f.commit()) {
        if (error) *error = tr("Couldn't write %1: %2").arg(path_, f.errorString());
        return false;
    }
    text_ = text;
    saved_ = config;
    setDirty(false);
    watch();
    return true;
}

void SettingsModel::revert() {
    config = saved_;
    setDirty(false);
    emit refreshNeeded();
}

bool SettingsModel::addEntry(bool allowList, const QString& entry) {
    const std::string e = entry.trimmed().toStdString();
    if (e.empty()) return false;
    std::vector<std::string>& list = allowList ? config.allow : config.block;
    if (std::find(list.begin(), list.end(), e) != list.end()) return false;
    list.push_back(e);
    changed();
    emit refreshNeeded();
    return true;
}
