// SPDX-License-Identifier: AGPL-3.0-or-later
#include "main_window.h"

#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "daemon_client.h"
#include "pages.h"
#include "platform.h"
#include "settings_model.h"
#include "widgets.h"

namespace {
const char* const kNavIcons[] = {"status", "activity", "settings", "lists"};
}

MainWindow::MainWindow(DaemonClient* client, SettingsModel* model, QWidget* parent)
    : QMainWindow(parent), client_(client), model_(model) {
    setWindowTitle(tr("FocusGuard Settings"));
    setWindowIcon(ui::appIcon());
    setMinimumSize(820, 560);
    resize(980, 680);

    auto* central = new QWidget;
    auto* root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Sidebar.
    auto* sidebar = new QWidget;
    sidebar->setObjectName("sidebar");
    sidebar->setAttribute(Qt::WA_StyledBackground);
    sidebar->setFixedWidth(208);
    auto* sv = new QVBoxLayout(sidebar);
    sv->setContentsMargins(0, 18, 0, 16);
    sv->setSpacing(14);
    auto* brand = new QHBoxLayout;
    brand->setContentsMargins(20, 0, 16, 0);
    brand->setSpacing(10);
    auto* logo = new ui::ShieldWidget(28);
    logo->setState(ui::ShieldState::Active);
    brand->addWidget(logo);
    auto* name = new QLabel("FocusGuard");
    name->setObjectName("brand");
    brand->addWidget(name, 1);
    sv->addLayout(brand);
    sv->addSpacing(6);

    nav_ = new QListWidget;
    nav_->setObjectName("nav");
    nav_->setIconSize(QSize(20, 20));
    nav_->setFocusPolicy(Qt::NoFocus);
    nav_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (const QString& label : {tr("Status"), tr("Activity"), tr("Settings"), tr("Lists")}) nav_->addItem(label);
    sv->addWidget(nav_, 1);
    connection_ = new QLabel;
    connection_->setContentsMargins(20, 0, 16, 0);
    sv->addWidget(connection_);

    // Pages, with the error banner above and the save bar below.
    auto* right = new QWidget;
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(0);
    loadError_ = new QLabel;
    loadError_->setObjectName("banner");
    loadError_->setWordWrap(true);
    auto* errorWrap = new QHBoxLayout;
    errorWrap->setContentsMargins(28, 16, 28, 0);
    errorWrap->addWidget(loadError_);
    rv->addLayout(errorWrap);

    pages_ = new QStackedWidget;
    auto* activity = new ActivityPage(client_, model_);
    pages_->addWidget(new StatusPage(client_));
    pages_->addWidget(activity);
    pages_->addWidget(new SettingsPage(client_, model_));
    pages_->addWidget(new ListsPage(client_, model_));
    rv->addWidget(pages_, 1);

    saveBar_ = new QFrame;
    saveBar_->setObjectName("saveBar");
    auto* sb = new QHBoxLayout(saveBar_);
    sb->setContentsMargins(28, 10, 28, 10);
    saveText_ = new QLabel(tr("You have unsaved changes."));
    sb->addWidget(saveText_, 1);
    auto* discard = ui::button(tr("Discard"));
    auto* saveBtn = ui::button(tr("Save and apply"), true);
    saveBtn->setDefault(true);
    sb->addWidget(discard);
    sb->addWidget(saveBtn);
    rv->addWidget(saveBar_);

    root->addWidget(sidebar);
    root->addWidget(right, 1);
    setCentralWidget(central);

    connect(nav_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    connect(discard, &QPushButton::clicked, model_, &SettingsModel::revert);
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::save);
    connect(model_, &SettingsModel::dirtyChanged, this, &MainWindow::updateSaveBar);
    connect(model_, &SettingsModel::refreshNeeded, this, &MainWindow::updateSaveBar);
    connect(client_, &DaemonClient::connectionChanged, this, &MainWindow::updateConnection);
    connect(client_, &DaemonClient::statusChanged, this, [this](const QJsonObject& s) {
        // Edit the file the running daemon actually reads.
        const QString path = s.value("config").toString();
        if (!path.isEmpty()) model_->setPath(path);
        updateConnection();
    });
    connect(activity, &ActivityPage::listsChanged, this, [this] { saveText_->setText(tr("Added to your lists. Save to apply.")); });

    model_->setPath(platform::defaultConfigPath());
    applyTheme();
    nav_->setCurrentRow(0);
    updateConnection();
    updateSaveBar();
}

void MainWindow::showPage(int index) { nav_->setCurrentRow(index); }
int MainWindow::pageCount() const { return pages_->count(); }

void MainWindow::applyTheme() {
    themeBusy_ = true;  // setStyleSheet sends change events of its own
    const ui::Theme t = ui::Theme::current();
    qApp->setStyleSheet(ui::styleSheet(t));
    for (int i = 0; i < nav_->count(); ++i) nav_->item(i)->setIcon(ui::navIcon(kNavIcons[i], t));
    themeBusy_ = false;
    updateConnection();
}

void MainWindow::changeEvent(QEvent* e) {
    if (!themeBusy_ && (e->type() == QEvent::PaletteChange || e->type() == QEvent::ThemeChange)) applyTheme();
    QMainWindow::changeEvent(e);
}

void MainWindow::updateConnection() {
    const ui::Theme t = ui::Theme::current();
    const QJsonObject s = client_->status();
    QString text, color;
    if (!client_->isConnected()) text = tr("Not running"), color = t.muted.name();
    else if (s.value("paused").toBool()) text = tr("Paused"), color = t.warn.name();
    else if (s.value("dry_run").toBool()) text = tr("Dry run"), color = t.warn.name();
    else text = tr("Protecting"), color = t.good.name();
    connection_->setText(QStringLiteral("<span style='color:%1'>●</span>&nbsp; %2").arg(color, text.toHtmlEscaped()));
}

void MainWindow::updateSaveBar() {
    saveBar_->setVisible(model_->dirty());
    if (!model_->dirty()) saveText_->setText(tr("You have unsaved changes."));
    loadError_->setVisible(!model_->loadError().isEmpty());
    loadError_->setText(tr("The settings file has an error, so changes can't be saved here until it's fixed in "
                           "a text editor:\n%1")
                            .arg(model_->loadError()));
}

bool MainWindow::save() {
    QString err;
    if (!model_->save(&err)) {
        QMessageBox::warning(this, tr("FocusGuard"), err);
        return false;
    }
    client_->send("reload");  // no-op while the daemon isn't running; it reads the file at start
    return true;
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (model_->dirty()) {
        const auto r = QMessageBox::question(this, tr("Unsaved changes"), tr("Save your changes before closing?"),
                                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                             QMessageBox::Save);
        if (r == QMessageBox::Cancel || (r == QMessageBox::Save && !save())) {
            e->ignore();
            return;
        }
    }
    e->accept();
}
