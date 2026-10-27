// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard Settings - the four pages (Status, Activity, Settings, Lists) and
// the window picker.
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>
#include <QWidget>

class DaemonClient;
class SettingsModel;
class UpdateCheck;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace ui {
class BarList;
class Card;
class ShieldWidget;
class Switch;
}  // namespace ui

class StatusPage : public QWidget {
    Q_OBJECT
public:
    StatusPage(DaemonClient* client, QWidget* parent = nullptr);

private:
    void refresh();
    void startDaemon();
    void finishWelcome();

    DaemonClient* client_;
    UpdateCheck* updates_;
    ui::ShieldWidget* shield_;
    QLabel *title_, *subtitle_, *blocked_, *allTime_, *allTimeCaption_, *noBlocks_, *details_, *overrides_,
        *updateText_;
    QWidget* allTimeBox_;
    QFrame* updateBanner_;
    ui::Card* welcome_;
    QPushButton *pause_, *startStop_, *reset_, *welcomeStart_;
    QMenu* pauseMenu_;
    ui::BarList* bars_;
    ui::Switch *autostart_, *checkUpdates_;
    QString updateUrl_;
};

class ActivityPage : public QWidget {
    Q_OBJECT
public:
    ActivityPage(DaemonClient* client, SettingsModel* model, QWidget* parent = nullptr);

signals:
    void listsChanged();  // an entry was added from the context menu

private:
    void add(const QJsonObject& e, bool atTop);
    void rebuild();
    void showMenu(const QPoint& pos);

    DaemonClient* client_;
    SettingsModel* model_;
    QVector<QJsonObject> events_;  // oldest first
    uint32_t grace_ = 0;           // the grace period the hints refer to
    QTreeWidget* tree_;
    QLabel* empty_;
    ui::Switch* showAllowed_;
};

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    SettingsPage(DaemonClient* client, SettingsModel* model, QWidget* parent = nullptr);

private:
    void refresh();  // model -> controls
    void apply();    // controls -> model
    void updateOverrides();

    DaemonClient* client_;
    SettingsModel* model_;
    QFrame* overridesBanner_;
    QLabel* overridesText_;
    bool refreshing_ = false;
    QSpinBox *grace_, *launchGrace_, *fightLimit_, *cooldown_;
    ui::Switch *strict_, *sameApp_, *related_, *launched_, *minimize_, *notify_, *dryRun_, *tray_, *motion_,
        *history_, *background_ = nullptr;
    QComboBox* verbosity_;
    QLineEdit* logFile_;
};

// One editable list (allow or block) with add / pick / remove buttons.
class ListEditor : public QWidget {
    Q_OBJECT
public:
    ListEditor(bool allowList, DaemonClient* client, SettingsModel* model, QWidget* parent = nullptr);
    void refresh();

private:
    void apply();

    bool allow_;
    DaemonClient* client_;
    SettingsModel* model_;
    QListWidget* list_;
    bool refreshing_ = false;
};

class ListsPage : public QWidget {
    Q_OBJECT
public:
    ListsPage(DaemonClient* client, SettingsModel* model, QWidget* parent = nullptr);
};

// Shows the open windows; the chosen one becomes an allow or block entry.
class WindowPicker : public QDialog {
    Q_OBJECT
public:
    WindowPicker(DaemonClient* client, bool allowList, QWidget* parent = nullptr);
    QString entry() const { return entry_; }
    bool allowList() const { return allowList_; }

private:
    void fill(const QJsonArray& windows);
    void updatePreview();
    QString makeEntry() const;

    DaemonClient* client_;
    QTreeWidget* tree_;
    QComboBox* matchBy_;
    QLabel *preview_, *offline_;
    QPushButton *addAllow_, *addBlock_;
    QString entry_;
    bool allowList_;
};
