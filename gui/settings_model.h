// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard Settings - the settings file being edited. Pages change
// `config` and call changed(); save() rewrites the file in place (keeping
// comments) with fg::write_config. Outside edits are picked up live.
#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>

#include <string>

#include "common.hpp"

class SettingsModel : public QObject {
    Q_OBJECT
public:
    explicit SettingsModel(QObject* parent = nullptr);

    fg::Config config;  // what the pages edit

    QString path() const { return path_; }
    void setPath(const QString& path);  // switches files (only when nothing is unsaved)
    QString loadError() const { return loadError_; }
    bool canSave() const { return loadError_.isEmpty(); }
    bool dirty() const { return dirty_; }

    void changed();  // call after editing `config`
    bool save(QString* error);
    void revert();

    // Adds an allow/block entry unless it is already there. Returns false if it was.
    bool addEntry(bool allowList, const QString& entry);

signals:
    void refreshNeeded();  // `config` changed underneath the pages: re-read it into the controls
    void dirtyChanged(bool dirty);

private:
    void load();
    void watch();
    void setDirty(bool d);

    QString path_, loadError_;
    std::string text_;  // the file as last read or written
    fg::Config saved_;  // what text_ holds
    bool dirty_ = false;
    QFileSystemWatcher watcher_;
};
