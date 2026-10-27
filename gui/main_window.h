// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard Settings - the main window: sidebar navigation, the pages and
// the save bar that appears while there are unsaved changes.
#pragma once

#include <QMainWindow>

class DaemonClient;
class SettingsModel;
class QFrame;
class QLabel;
class QListWidget;
class QStackedWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(DaemonClient* client, SettingsModel* model, QWidget* parent = nullptr);
    void showPage(int index);
    int pageCount() const;

protected:
    void closeEvent(QCloseEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    void applyTheme();
    void updateConnection();
    void updateSaveBar();
    bool save();

    DaemonClient* client_;
    SettingsModel* model_;
    QListWidget* nav_;
    QStackedWidget* pages_;
    QLabel *connection_, *saveText_, *loadError_;
    QFrame* saveBar_;
    bool themeBusy_ = false;
};
