// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pages.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "daemon_client.h"
#include "platform.h"
#include "settings_model.h"
#include "update_check.h"
#include "widgets.h"

namespace {

// A page: title, optional intro, then the content in a scroll area.
QVBoxLayout* pageLayout(QWidget* page, const QString& title, const QString& intro) {
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    content->setObjectName("pageContent");
    content->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setWidget(content);
    outer->addWidget(scroll);
    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(28, 22, 28, 24);
    v->setSpacing(14);
    v->addWidget(ui::pageTitle(title));
    if (!intro.isEmpty()) v->addWidget(ui::hintLabel(intro));
    return v;
}

QSpinBox* msSpin(int max, int step) {
    auto* s = new QSpinBox;
    s->setRange(0, max);
    s->setSingleStep(step);
    s->setSuffix(" ms");
    s->setGroupSeparatorShown(true);
    s->setMinimumWidth(130);
    s->setAlignment(Qt::AlignRight);
    return s;
}

QString kindLabel(const QString& kind) {
    if (kind == "blocked") return QObject::tr("Blocked");
    if (kind == "allowed") return QObject::tr("Allowed");
    if (kind == "would-block") return QObject::tr("Would block");
    if (kind == "gave-up") return QObject::tr("Gave up");
    if (kind == "failed") return QObject::tr("Failed");
    return kind;
}

QColor kindColor(const QString& kind, const ui::Theme& t) {
    if (kind == "blocked" || kind == "failed") return t.bad;
    if (kind == "allowed") return t.good;
    if (kind == "would-block") return t.warn;
    return t.muted;
}

// The list entry that matches an app by name: the process on Windows (minus
// ".exe"), the WM_CLASS on Linux.
QString appEntry(const QJsonObject& o) {
    QString e = o.value("class").toString();
    const QString process = o.value("process").toString();
#ifdef Q_OS_WIN
    if (!process.isEmpty()) e = process;
#else
    if (e.isEmpty()) e = process;
#endif
    if (e.endsWith(".exe", Qt::CaseInsensitive)) e.chop(4);
    return e;
}

// "0.4 s", "12 s": a delay as people read it.
QString seconds(double ms) {
    if (ms < 10000) return QObject::tr("%1 s").arg(QLocale().toString(ms / 1000.0, 'f', 1));
    return QObject::tr("%1 s").arg(qRound(ms / 1000.0));
}

// How close a decision came to the grace period, for tuning it. Returns the
// cell text, sets `hint` to an explanation and `near` when it was close.
QString timing(const QJsonObject& e, uint32_t grace, QString& hint, bool& near) {
    const double after = e.value("after").toDouble(-1);
    near = false;
    hint.clear();
    if (after < 0) return QStringLiteral("–");
    if (after >= 60000) return QObject::tr("over a minute");
    const QString kind = e.value("kind").toString(), reason = e.value("reason").toString();
    const QString a = seconds(after), g = seconds(grace);
    if (kind == "allowed" && reason == "user action") {
        near = after >= grace / 2.0;
        hint = QObject::tr("This came %1 after your last click, Alt+Tab, Enter or similar: within your grace "
                           "period of %2, so FocusGuard took it as your doing. If you didn't ask for this window, "
                           "a grace period shorter than %1 would have blocked it.")
                   .arg(a, g);
    } else if ((kind == "blocked" || kind == "would-block") && reason == "no user action" && after <= grace + 3000.0) {
        near = true;
        hint = QObject::tr("This came %1 after your last click, Alt+Tab, Enter or similar: just after your grace "
                           "period of %2. If it was a window you were waiting for, a grace period longer than %1 "
                           "would have let it through.")
                   .arg(a, g);
    } else {
        hint = QObject::tr("%1 after your last click, Alt+Tab, Enter or similar. Your grace period is %2.").arg(a, g);
    }
    return a;
}

// A path for display: the home directory shortened to ~ (Linux).
QString displayPath(const QString& path) {
#ifndef Q_OS_WIN
    const QString home = QDir::homePath();
    if (path.startsWith(home + "/")) return "~" + path.mid(home.size());
#endif
    return QDir::toNativeSeparators(path);
}

QString toggleName(const QString& key) {
    if (key == "strict") return QObject::tr("Strict mode");
    if (key == "minimize-on-block") return QObject::tr("Minimise blocked windows");
    if (key == "notify") return QObject::tr("Notifications");
    if (key == "dry-run") return QObject::tr("Dry run");
    return key;
}

}  // namespace

// ------------------------------------------------------------------ Status

StatusPage::StatusPage(DaemonClient* client, QWidget* parent)
    : QWidget(parent), client_(client), updates_(new UpdateCheck(this)) {
    auto* v = pageLayout(this, tr("Status"), {});

    // First run: offer to start FocusGuard now and at every login.
    welcome_ = new ui::Card(tr("Welcome to FocusGuard"),
                            tr("FocusGuard works quietly in the background: it stops other windows from taking "
                               "your keyboard focus while you type or click. It needs to be running to do that."));
    auto* wb = new QHBoxLayout;
    wb->setSpacing(8);
    welcomeStart_ = ui::button({}, true);
    auto* welcomeLater = ui::button(tr("Not now"));
    wb->addWidget(welcomeStart_);
    wb->addWidget(welcomeLater);
    wb->addStretch();
    welcome_->body()->addLayout(wb);
    v->addWidget(welcome_);
    // Seen it before, or already set to start at login (by the installer, say).
    const bool welcomed = platform::preferences().value("welcomed", false).toBool() || platform::autostartEnabled();
    welcome_->setVisible(!welcomed);
    if (welcomed) platform::preferences().setValue("welcomed", true);

    // A newer release, when the update check is on and found one.
    updateBanner_ = new QFrame;
    updateBanner_->setObjectName("banner");
    updateBanner_->setAttribute(Qt::WA_StyledBackground);
    auto* ub = new QHBoxLayout(updateBanner_);
    ub->setContentsMargins(0, 0, 0, 0);
    updateText_ = new QLabel;
    updateText_->setWordWrap(true);
    ub->addWidget(updateText_, 1);
    auto* download = ui::button(tr("Download"));
    ub->addWidget(download, 0, Qt::AlignVCenter);
    v->addWidget(updateBanner_);

    // Hero: shield, state and the main actions.
    auto* hero = new ui::Card;
    auto* h = new QHBoxLayout;
    h->setSpacing(20);
    shield_ = new ui::ShieldWidget(76);
    h->addWidget(shield_, 0, Qt::AlignTop);
    auto* text = new QVBoxLayout;
    text->setSpacing(4);
    title_ = new QLabel;
    title_->setObjectName("heroTitle");
    subtitle_ = ui::hintLabel({});
    text->addWidget(title_);
    text->addWidget(subtitle_);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    pause_ = ui::button({}, true);
    pauseMenu_ = new QMenu(pause_);
    pauseMenu_->addAction(tr("Until I resume"), this, [this] { client_->send("pause"); });
    pauseMenu_->addAction(tr("For 15 minutes"), this, [this] { client_->send("pause 15"); });
    pauseMenu_->addAction(tr("For 1 hour"), this, [this] { client_->send("pause 60"); });
    startStop_ = ui::button({});
    buttons->addWidget(pause_);
    buttons->addWidget(startStop_);
    buttons->addStretch();
    text->addSpacing(8);
    text->addLayout(buttons);
    h->addLayout(text, 1);
    hero->body()->addLayout(h);
    v->addWidget(hero);

    overrides_ = new QLabel(tr("FocusGuard was started with command-line options. They take precedence over "
                               "the settings below and in the settings file."));
    overrides_->setObjectName("banner");
    overrides_->setWordWrap(true);
    v->addWidget(overrides_);

    // Statistics.
    auto* stats = new ui::Card(tr("Blocked windows"), tr("Windows that tried to take focus while you were busy elsewhere."));
    auto* numbers = new QHBoxLayout;
    numbers->setSpacing(28);
    auto number = [&](QLabel*& value, QLabel*& caption) {
        auto* box = new QWidget;
        auto* bv = new QVBoxLayout(box);
        bv->setContentsMargins(0, 0, 0, 0);
        bv->setSpacing(0);
        value = new QLabel("0");
        value->setObjectName("bigNumber");
        caption = ui::hintLabel({});
        bv->addWidget(value);
        bv->addWidget(caption);
        numbers->addWidget(box);
        return box;
    };
    QLabel* sessionCaption = nullptr;
    number(blocked_, sessionCaption);
    sessionCaption->setText(tr("this session"));
    allTimeBox_ = number(allTime_, allTimeCaption_);
    numbers->addStretch();
    reset_ = ui::button(tr("Reset"));
    reset_->setToolTip(tr("Start the statistics over"));
    numbers->addWidget(reset_, 0, Qt::AlignTop);
    stats->body()->addLayout(numbers);
    bars_ = new ui::BarList;
    stats->body()->addWidget(bars_);
    noBlocks_ = ui::hintLabel(tr("Nothing has been blocked yet."));
    stats->body()->addWidget(noBlocks_);
    v->addWidget(stats);

    // Startup and updates.
    auto* startup = new ui::Card(tr("Startup and updates"));
    autostart_ = new ui::Switch;
    autostart_->setChecked(platform::autostartEnabled());
    startup->body()->addWidget(ui::settingRow(tr("Start FocusGuard when you log in"),
                                          tr("Runs it in the background, with its tray icon, every time you sign in."),
                                          autostart_));
    checkUpdates_ = new ui::Switch;
    checkUpdates_->setChecked(updates_->enabled());
    startup->body()->addWidget(ui::settingRow(
        tr("Check for new versions"),
        tr("Once a day, this app asks GitHub whether a newer FocusGuard exists. GitHub sees your IP address; "
           "nothing else is sent."),
        checkUpdates_));
    v->addWidget(startup);

    details_ = ui::hintLabel({});
    details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(details_);
    v->addStretch();

    // While running, the button opens the pause menu; while paused, it resumes.
    connect(pause_, &QPushButton::clicked, this, [this] {
        if (client_->status().value("paused").toBool()) client_->send("resume");
    });
    connect(startStop_, &QPushButton::clicked, this, [this] {
        if (client_->isConnected()) client_->send("quit");
        else startDaemon();
    });
    connect(welcomeStart_, &QPushButton::clicked, this, [this] {
        autostart_->setChecked(true);           // its handler enables starting at login
        if (!autostart_->isChecked()) return;  // it couldn't, and said why
        if (!client_->isConnected()) startDaemon();
        finishWelcome();
    });
    connect(welcomeLater, &QPushButton::clicked, this, &StatusPage::finishWelcome);
    connect(checkUpdates_, &QAbstractButton::toggled, updates_, &UpdateCheck::setEnabled);
    connect(updates_, &UpdateCheck::changed, this, &StatusPage::refresh);
    connect(download, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl(updates_->releasePage())); });
    connect(reset_, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Reset statistics"), tr("Start the statistics over? This can't be undone.")) ==
            QMessageBox::Yes)
            client_->send("reset-stats");
    });
    connect(autostart_, &QAbstractButton::toggled, this, [this](bool on) {
        QString err;
        if (!platform::setAutostart(on, &err)) {
            QMessageBox::warning(this, tr("FocusGuard"), err);
            QSignalBlocker block(autostart_);
            autostart_->setChecked(platform::autostartEnabled());
        }
    });
    connect(client_, &DaemonClient::statusChanged, this, &StatusPage::refresh);
    connect(client_, &DaemonClient::connectionChanged, this, &StatusPage::refresh);
    refresh();
    updates_->checkIfDue();
}

void StatusPage::startDaemon() {
    QString err;
    if (!platform::startDaemon(&err)) {
        QMessageBox::warning(this, tr("FocusGuard"), err);
        return;
    }
    startStop_->setEnabled(false);
    startStop_->setText(tr("Starting…"));
    client_->reconnectSoon(400);
    QTimer::singleShot(4000, this, [this] { refresh(); });  // give up waiting visibly
}

void StatusPage::finishWelcome() {
    platform::preferences().setValue("welcomed", true);
    welcome_->hide();
}

void StatusPage::refresh() {
    const QJsonObject s = client_->status();
    const bool running = client_->isConnected() && !s.isEmpty();
    const bool paused = s.value("paused").toBool(), dry = s.value("dry_run").toBool();
    startStop_->setEnabled(true);
    if (!running) {
        shield_->setState(ui::ShieldState::Off);
        title_->setText(tr("FocusGuard isn't running"));
        subtitle_->setText(tr("Windows can steal your keyboard focus until you start it."));
        pause_->hide();
        startStop_->setText(tr("Start FocusGuard"));
        startStop_->setObjectName("primary");
    } else {
        shield_->setState(paused ? ui::ShieldState::Paused : dry ? ui::ShieldState::DryRun : ui::ShieldState::Active);
        title_->setText(paused ? tr("Paused") : dry ? tr("Dry run") : tr("Protecting your focus"));
        const qint64 until = (qint64)s.value("paused_until").toDouble();
        const QString untilText =
            QLocale().toString(QDateTime::fromMSecsSinceEpoch(until).time(), QLocale::ShortFormat);
        subtitle_->setText(paused && until ? tr("Any window may take focus until %1, when protection resumes by itself.")
                                                 .arg(untilText)
                           : paused ? tr("Any window may take focus until you resume.")
                           : dry  ? tr("Focus steals are logged on the Activity page but not blocked.")
                           : s.value("strict").toBool()
                               ? tr("Strict mode: only your own actions and the allow list may change focus.")
                               : tr("Windows only get focus when you switch to them."));
        pause_->show();
        pause_->setText(paused ? tr("Resume") : tr("Pause"));
        pause_->setMenu(paused ? nullptr : pauseMenu_);
        startStop_->setText(tr("Stop FocusGuard"));
        startStop_->setObjectName("secondary");
    }
    startStop_->style()->unpolish(startStop_);  // objectName changed: re-apply the stylesheet
    startStop_->style()->polish(startStop_);
    overrides_->setVisible(running && s.value("cli_overrides").toBool());
    welcomeStart_->setText(running ? tr("Start it every time I log in") : tr("Start now and every time I log in"));
    const QString newer = updates_->newerVersion();
    updateText_->setText(tr("FocusGuard %1 is available. You have %2.").arg(newer, fg::kVersion));
    updateBanner_->setVisible(!newer.isEmpty());

    blocked_->setText(QString::number(s.value("blocked").toInt()));
    // With the history on, the all-time numbers are the interesting ones.
    const bool history = s.value("history").toBool();
    const QDateTime since = QDateTime::fromMSecsSinceEpoch((qint64)s.value("since").toDouble());
    allTime_->setText(QString::number(s.value("blocked_total").toInt()));
    allTimeCaption_->setText(tr("since %1").arg(QLocale().toString(since.date(), QLocale::ShortFormat)));
    allTimeBox_->setVisible(running && history);
    reset_->setVisible(running);
    QVector<QPair<QString, int>> items;
    for (const QJsonValue& t : s.value(history ? "top_total" : "top").toArray()) {
        if (items.size() == 5) break;
        items.append({t.toObject().value("app").toString(), t.toObject().value("count").toInt()});
    }
    bars_->setItems(items);
    bars_->setVisible(!items.isEmpty());
    noBlocks_->setVisible(items.isEmpty());
    noBlocks_->setText(running ? tr("Nothing has been blocked yet.") : tr("Statistics appear while FocusGuard runs."));

    QString details = tr("FocusGuard Settings %1").arg(fg::kVersion);
    if (running) {
        details += " · " + tr("daemon %1, process %2").arg(s.value("version").toString()).arg(s.value("pid").toInt());
        details += "\n" + tr("Settings file: %1").arg(displayPath(s.value("config").toString()));
    }
    details_->setText(details);
}

// ------------------------------------------------------------------ Activity

ActivityPage::ActivityPage(DaemonClient* client, SettingsModel* model, QWidget* parent)
    : QWidget(parent), client_(client), model_(model) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(28, 22, 28, 24);
    v->setSpacing(12);
    v->addWidget(ui::pageTitle(tr("Activity")));
    v->addWidget(ui::hintLabel(tr("Focus changes FocusGuard judged, newest first. Right-click an entry to always "
                                  "allow or block that app. Times in orange came close to your grace period; "
                                  "hover over them for a tuning hint.")));

    auto* bar = new QHBoxLayout;
    showAllowed_ = new ui::Switch;
    bar->addWidget(showAllowed_);
    auto* label = new QLabel(tr("Show allowed changes too"));
    bar->addWidget(label);
    bar->addStretch();
    auto* clear = ui::button(tr("Clear history"));
    bar->addWidget(clear);
    v->addLayout(bar);

    auto* card = new ui::Card;
    card->body()->setContentsMargins(1, 1, 1, 1);
    tree_ = new QTreeWidget;
    tree_->setObjectName("table");
    tree_->setHeaderLabels({tr("Time"), tr("Result"), tr("App"), tr("Window"), tr("Reason"), tr("After you")});
    tree_->headerItem()->setToolTip(5, tr("How long after your last click, Alt+Tab, Enter or similar it happened"));
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(false);
    tree_->setFrameShape(QFrame::NoFrame);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    tree_->setColumnWidth(0, 96);
    tree_->setColumnWidth(1, 108);
    tree_->setColumnWidth(2, 120);
    tree_->setColumnWidth(4, 150);
    tree_->setColumnWidth(5, 100);
    card->body()->addWidget(tree_);
    empty_ = ui::hintLabel({});
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setContentsMargins(0, 24, 0, 24);
    card->body()->addWidget(empty_);
    v->addWidget(card, 1);

    connect(showAllowed_, &QAbstractButton::toggled, this, &ActivityPage::rebuild);
    connect(clear, &QPushButton::clicked, this, [this] {
        if (client_->isConnected()) {
            if (QMessageBox::question(this, tr("Clear history"),
                                      tr("Remove all recorded activity, including the history kept on disk?")) !=
                QMessageBox::Yes)
                return;
            client_->send("clear-history");
        }
        events_.clear();
        rebuild();
    });
    connect(tree_, &QWidget::customContextMenuRequested, this, &ActivityPage::showMenu);
    connect(client_, &DaemonClient::historyReceived, this, [this](const QJsonArray& items) {
        events_.clear();
        for (const QJsonValue& e : items) events_.append(e.toObject());
        rebuild();
    });
    connect(client_, &DaemonClient::eventReceived, this, [this](const QJsonObject& e) {
        events_.append(e);
        if (events_.size() > 2000) events_.removeFirst();
        add(e, true);
        empty_->setVisible(tree_->topLevelItemCount() == 0);
        tree_->setVisible(!empty_->isVisible());
        while (tree_->topLevelItemCount() > 2000) delete tree_->takeTopLevelItem(tree_->topLevelItemCount() - 1);
    });
    connect(client_, &DaemonClient::connectionChanged, this, &ActivityPage::rebuild);
    // The hints compare with the grace period in the settings file.
    grace_ = model_->config.grace_ms;
    auto graceChanged = [this] {  // once saved: unsaved edits aren't in effect yet
        if (model_->dirty() || model_->config.grace_ms == grace_) return;
        grace_ = model_->config.grace_ms;
        rebuild();
    };
    connect(model_, &SettingsModel::refreshNeeded, this, graceChanged);
    connect(model_, &SettingsModel::dirtyChanged, this, graceChanged);
    rebuild();
}

void ActivityPage::add(const QJsonObject& e, bool atTop) {
    const QString kind = e.value("kind").toString();
    if (kind == "allowed" && !showAllowed_->isChecked()) return;
    const ui::Theme t = ui::Theme::current();
    const QDateTime when = QDateTime::fromMSecsSinceEpoch((qint64)e.value("time").toDouble());
    // Today: just the time. Earlier (from the saved history): the date too.
    const QString time = when.date() == QDate::currentDate()
                             ? when.toString("HH:mm:ss")
                             : QLocale().toString(when.date(), "d MMM") + " " + when.toString("HH:mm");
    QString hint;
    bool near = false;
    const QString after = timing(e, grace_, hint, near);
    auto* item = new QTreeWidgetItem({time, QStringLiteral("●  ") + kindLabel(kind),
                                      e.value("app").toString(), e.value("title").toString(),
                                      e.value("reason").toString(), after});
    item->setForeground(1, kindColor(kind, t));
    if (near) item->setForeground(5, t.warn);
    item->setToolTip(5, hint);
    item->setToolTip(4, hint);
    item->setToolTip(3, e.value("title").toString());
    item->setToolTip(0, when.toString(Qt::TextDate));
    item->setData(0, Qt::UserRole, e);
    if (atTop) tree_->insertTopLevelItem(0, item);
    else tree_->addTopLevelItem(item);
}

void ActivityPage::rebuild() {
    tree_->clear();
    for (int i = (int)events_.size() - 1; i >= 0; --i) add(events_[i], false);
    const bool none = tree_->topLevelItemCount() == 0;
    empty_->setText(!client_->isConnected() ? tr("FocusGuard isn't running, so there is no activity to show.")
                    : events_.isEmpty()     ? tr("Nothing yet. Focus changes appear here as they happen.")
                                            : tr("Nothing was blocked. Turn on “Show allowed changes too” to see the rest."));
    empty_->setVisible(none);
    tree_->setVisible(!none);
}

void ActivityPage::showMenu(const QPoint& pos) {
    QTreeWidgetItem* item = tree_->itemAt(pos);
    if (!item) return;
    const QJsonObject e = item->data(0, Qt::UserRole).toJsonObject();
    const QString entry = appEntry(e);
    QMenu menu(this);
    QAction* allow = menu.addAction(tr("Always allow “%1”").arg(entry));
    QAction* block = menu.addAction(tr("Never let “%1” take focus on its own").arg(entry));
    allow->setEnabled(!entry.isEmpty());
    block->setEnabled(!entry.isEmpty());
    menu.addSeparator();
    QAction* copy = menu.addAction(tr("Copy window title"));
    QAction* chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
    if (chosen == copy) {
        QApplication::clipboard()->setText(e.value("title").toString());
    } else if (chosen == allow || chosen == block) {
        if (model_->addEntry(chosen == allow, entry)) emit listsChanged();
    }
}

// ------------------------------------------------------------------ Settings

SettingsPage::SettingsPage(DaemonClient* client, SettingsModel* model, QWidget* parent)
    : QWidget(parent), client_(client), model_(model) {
    auto* v = pageLayout(this, tr("Settings"),
                         tr("Changes are saved to the settings file and applied to the running FocusGuard."));

    // Tray toggles that differ from the file: offer to make them permanent.
    overridesBanner_ = new QFrame;
    overridesBanner_->setObjectName("banner");
    overridesBanner_->setAttribute(Qt::WA_StyledBackground);
    auto* ob = new QHBoxLayout(overridesBanner_);
    ob->setContentsMargins(0, 0, 0, 0);
    overridesText_ = new QLabel;
    overridesText_->setWordWrap(true);
    ob->addWidget(overridesText_, 1);
    auto* keep = ui::button(tr("Keep in the settings file"));
    ob->addWidget(keep, 0, Qt::AlignVCenter);
    v->addWidget(overridesBanner_);
    connect(keep, &QPushButton::clicked, this, [this] {
        for (const QJsonValue& o : client_->status().value("overrides").toArray()) {
            fg::Toggle t;
            if (fg::toggle_from_key(o.toObject().value("key").toString().toStdString(), t))
                fg::set_toggle(model_->config, t, o.toObject().value("value").toBool());
        }
        model_->changed();
        refresh();
        updateOverrides();
    });

    auto* timing = new ui::Card(tr("Your own actions"),
                                tr("A click, Enter, Esc or an Alt/Win shortcut shortly before a focus change "
                                   "means you asked for it. Plain typing never counts."));
    grace_ = msSpin(fg::kMaxMs, 100);
    timing->body()->addWidget(ui::settingRow(tr("Grace period"), tr("How long after such an action focus may change."), grace_));
    launchGrace_ = msSpin(fg::kMaxMs, 1000);
    timing->body()->addWidget(ui::settingRow(tr("Startup time for apps you launch"),
                                             tr("An app you just started may take focus this long while it loads, unless you switch to another window first. 0 turns this off."),
                                             launchGrace_));
    v->addWidget(timing);

    auto* rules = new ui::Card(tr("Automatic exceptions"), tr("Focus changes that are allowed even without an action of yours."));
    strict_ = new ui::Switch;
    rules->body()->addWidget(ui::settingRow(tr("Strict mode"), tr("Turns the three exceptions below off: only your own "
                                                                  "actions and the allow list may change focus."), strict_));
    sameApp_ = new ui::Switch;
    rules->body()->addWidget(ui::settingRow(tr("Within the same app"), tr("An app may move focus between its own windows."), sameApp_));
    related_ = new ui::Switch;
    rules->body()->addWidget(ui::settingRow(tr("Dialogs of the current window"), tr("A dialog opened by the window you're in may take focus."), related_));
    launched_ = new ui::Switch;
    rules->body()->addWidget(ui::settingRow(tr("Apps you just launched"), tr("Uses the startup time above."), launched_));
    v->addWidget(rules);

    auto* onBlock = new ui::Card(tr("When a window is blocked"));
    minimize_ = new ui::Switch;
    onBlock->body()->addWidget(ui::settingRow(tr("Minimise it"), tr("Instead of only flashing its taskbar entry."), minimize_));
    notify_ = new ui::Switch;
    onBlock->body()->addWidget(ui::settingRow(tr("Show a notification"), {}, notify_));
    cooldown_ = msSpin(fg::kMaxMs, 1000);
    onBlock->body()->addWidget(ui::settingRow(tr("Quiet time between notifications"), tr("So a burst of blocks can't flood you."), cooldown_));
    fightLimit_ = new QSpinBox;
    fightLimit_->setRange(0, 1000);
    fightLimit_->setMinimumWidth(130);
    fightLimit_->setAlignment(Qt::AlignRight);
    fightLimit_->setSpecialValueText(tr("Never"));
    onBlock->body()->addWidget(ui::settingRow(tr("Give up after this many attempts in 3 s"),
                                              tr("A window that keeps grabbing focus wins in the end, rather than flickering forever."),
                                              fightLimit_));
    dryRun_ = new ui::Switch;
    onBlock->body()->addWidget(ui::settingRow(tr("Dry run"), tr("Only log what would be blocked; useful while tuning the lists."), dryRun_));
    v->addWidget(onBlock);

    auto* other = new ui::Card(tr("Other"));
    tray_ = new ui::Switch;
    other->body()->addWidget(ui::settingRow(tr("Tray icon"), tr("Show FocusGuard in the system tray."), tray_));
    motion_ = new ui::Switch;
    other->body()->addWidget(ui::settingRow(tr("Mouse movement counts as an action"),
                                            tr("For focus-follows-mouse setups."), motion_));
    history_ = new ui::Switch;
#ifdef Q_OS_WIN
    const QString historyDir = QStringLiteral("%LOCALAPPDATA%\\FocusGuard");
#else
    const QString historyDir = QStringLiteral("~/.local/state/focusguard");
#endif
    other->body()->addWidget(ui::settingRow(tr("Keep a history"),
                                            tr("Remember blocked windows and statistics across restarts, in %1.").arg(historyDir),
                                            history_));
#ifdef Q_OS_WIN
    background_ = new ui::Switch;
    other->body()->addWidget(ui::settingRow(tr("Hide the console window"),
                                            tr("For focusguard.exe; the tray-only build never shows one."), background_));
#endif
    verbosity_ = new QComboBox;
    verbosity_->addItems({tr("Errors only"), tr("Blocked windows"), tr("Every focus change")});
    other->body()->addWidget(ui::settingRow(tr("Log detail"), tr("What FocusGuard writes to its log."), verbosity_));
    auto* logRow = new QWidget;
    auto* lh = new QHBoxLayout(logRow);
    lh->setContentsMargins(0, 0, 0, 0);
    logFile_ = new QLineEdit;
    logFile_->setPlaceholderText(tr("No log file"));
    logFile_->setClearButtonEnabled(true);
    logFile_->setMinimumWidth(220);
    auto* browse = ui::button(tr("Browse…"));
    lh->addWidget(logFile_);
    lh->addWidget(browse);
    other->body()->addWidget(ui::settingRow(tr("Log file"), tr("Also append the log to this file."), logRow));
    v->addWidget(other);
    v->addStretch();

    for (QSpinBox* s : {grace_, launchGrace_, fightLimit_, cooldown_})
        connect(s, qOverload<int>(&QSpinBox::valueChanged), this, &SettingsPage::apply);
    for (ui::Switch* s : {sameApp_, related_, launched_, minimize_, notify_, dryRun_, tray_, motion_, history_})
        connect(s, &QAbstractButton::toggled, this, &SettingsPage::apply);
    if (background_) connect(background_, &QAbstractButton::toggled, this, &SettingsPage::apply);
    connect(strict_, &QAbstractButton::toggled, this, [this](bool on) {
        if (refreshing_) return;
        refreshing_ = true;  // strict is shorthand for the three rules
        sameApp_->setChecked(!on);
        related_->setChecked(!on);
        launched_->setChecked(!on);
        refreshing_ = false;
        apply();
    });
    connect(verbosity_, qOverload<int>(&QComboBox::currentIndexChanged), this, &SettingsPage::apply);
    connect(logFile_, &QLineEdit::textChanged, this, &SettingsPage::apply);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, tr("Log file"), logFile_->text(), {}, nullptr,
                                                       QFileDialog::DontConfirmOverwrite);
        if (!f.isEmpty()) logFile_->setText(QDir::toNativeSeparators(f));
    });
    connect(model_, &SettingsModel::refreshNeeded, this, &SettingsPage::refresh);
    connect(model_, &SettingsModel::refreshNeeded, this, &SettingsPage::updateOverrides);
    connect(client_, &DaemonClient::statusChanged, this, &SettingsPage::updateOverrides);
    refresh();
    updateOverrides();
}

// Lists the tray toggles whose value the settings file (as edited here) doesn't have.
void SettingsPage::updateOverrides() {
    QStringList pending;
    for (const QJsonValue& o : client_->status().value("overrides").toArray()) {
        const QString key = o.toObject().value("key").toString();
        const bool value = o.toObject().value("value").toBool();
        fg::Toggle t;
        if (fg::toggle_from_key(key.toStdString(), t) && fg::toggle_value(model_->config, t) != value)
            pending << tr("%1 %2").arg(toggleName(key), value ? tr("on") : tr("off"));
    }
    overridesText_->setText(tr("Changed from the tray menu but not in the settings file: %1. These stay in effect "
                               "until FocusGuard restarts or the file changes them.")
                                .arg(pending.join(", ")));
    overridesBanner_->setVisible(!pending.isEmpty());
}

void SettingsPage::refresh() {
    refreshing_ = true;
    const fg::Config& c = model_->config;
    grace_->setValue((int)c.grace_ms);
    launchGrace_->setValue((int)c.launch_grace_ms);
    fightLimit_->setValue((int)c.fight_limit);
    cooldown_->setValue((int)c.notify_cooldown_ms);
    sameApp_->setChecked(c.allow_same_app);
    related_->setChecked(c.allow_related);
    launched_->setChecked(c.allow_launched);
    strict_->setChecked(!c.allow_same_app && !c.allow_related && !c.allow_launched);
    minimize_->setChecked(c.minimize_on_block);
    notify_->setChecked(c.notify);
    cooldown_->setEnabled(c.notify);
    dryRun_->setChecked(c.dry_run);
    tray_->setChecked(c.tray);
    motion_->setChecked(c.count_motion);
    history_->setChecked(c.history);
    if (background_) background_->setChecked(c.background);
    verbosity_->setCurrentIndex(std::clamp(c.verbosity, 0, 2));
    logFile_->setText(QString::fromStdString(c.log_path));
    launchGrace_->setEnabled(c.allow_launched);
    refreshing_ = false;
}

void SettingsPage::apply() {
    if (refreshing_) return;
    fg::Config& c = model_->config;
    c.grace_ms = (uint32_t)grace_->value();
    c.launch_grace_ms = (uint32_t)launchGrace_->value();
    c.fight_limit = (uint32_t)fightLimit_->value();
    c.notify_cooldown_ms = (uint32_t)cooldown_->value();
    c.allow_same_app = sameApp_->isChecked();
    c.allow_related = related_->isChecked();
    c.allow_launched = launched_->isChecked();
    c.minimize_on_block = minimize_->isChecked();
    c.notify = notify_->isChecked();
    c.dry_run = dryRun_->isChecked();
    c.tray = tray_->isChecked();
    c.count_motion = motion_->isChecked();
    c.history = history_->isChecked();
    if (background_) c.background = background_->isChecked();
    c.verbosity = verbosity_->currentIndex();
    c.log_path = logFile_->text().trimmed().toStdString();
    {
        QSignalBlocker block(strict_);
        strict_->setChecked(!c.allow_same_app && !c.allow_related && !c.allow_launched);
    }
    cooldown_->setEnabled(c.notify);
    launchGrace_->setEnabled(c.allow_launched);
    model_->changed();
}

// ------------------------------------------------------------------ Lists

ListEditor::ListEditor(bool allowList, DaemonClient* client, SettingsModel* model, QWidget* parent)
    : QWidget(parent), allow_(allowList), client_(client), model_(model) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    auto* card = new ui::Card(allowList ? tr("Always allow") : tr("Never on their own"),
                              allowList ? tr("May take focus whenever they like.")
                                        : tr("Only get focus when you switch to them."));
    list_ = new QListWidget;
    list_->setObjectName("entries");
    list_->setMinimumHeight(200);
    list_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    card->body()->addWidget(list_, 1);
    auto* buttons = new QHBoxLayout;
    auto* add = ui::button(tr("Add"));
    auto* pick = ui::button(tr("Pick a window…"));
    auto* remove = ui::button(tr("Remove"));
    remove->setEnabled(false);
    buttons->addWidget(add);
    buttons->addWidget(pick);
    buttons->addStretch();
    buttons->addWidget(remove);
    card->body()->addLayout(buttons);
    v->addWidget(card);

    connect(add, &QPushButton::clicked, this, [this] {
        auto* item = new QListWidgetItem;
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        list_->addItem(item);
        list_->setCurrentItem(item);
        list_->editItem(item);
    });
    connect(pick, &QPushButton::clicked, this, [this] {
        WindowPicker picker(client_, allow_, this);
        if (picker.exec() == QDialog::Accepted) model_->addEntry(picker.allowList(), picker.entry());
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        qDeleteAll(list_->selectedItems());
        apply();
    });
    connect(list_, &QListWidget::itemSelectionChanged, this, [this, remove] {
        remove->setEnabled(!list_->selectedItems().isEmpty());
    });
    connect(list_, &QListWidget::itemChanged, this, [this] { apply(); });
    // An item left empty after editing is dropped.
    connect(list_->itemDelegate(), &QAbstractItemDelegate::closeEditor, this, [this] {
        for (int i = list_->count() - 1; i >= 0; --i)
            if (list_->item(i)->text().trimmed().isEmpty()) delete list_->takeItem(i);
        apply();
    });
    connect(model_, &SettingsModel::refreshNeeded, this, &ListEditor::refresh);
    refresh();
}

void ListEditor::refresh() {
    refreshing_ = true;
    list_->clear();
    for (const std::string& e : allow_ ? model_->config.allow : model_->config.block) {
        auto* item = new QListWidgetItem(QString::fromStdString(e));
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        list_->addItem(item);
    }
    refreshing_ = false;
}

void ListEditor::apply() {
    if (refreshing_) return;
    std::vector<std::string> entries;
    for (int i = 0; i < list_->count(); ++i) {
        // A comma would split the entry on load, so split it now, visibly.
        for (const QString& part : list_->item(i)->text().split(',', Qt::SkipEmptyParts)) {
            const QString e = part.trimmed();
            if (!e.isEmpty()) entries.push_back(e.toStdString());
        }
    }
    (allow_ ? model_->config.allow : model_->config.block) = entries;
    model_->changed();
}

ListsPage::ListsPage(DaemonClient* client, SettingsModel* model, QWidget* parent) : QWidget(parent) {
    auto* v = pageLayout(this, tr("Lists"),
                         tr("Each entry is an app name (the process name, or the window class on Linux), a "
                            "pattern with * and ? such as *updater*, or title: followed by a window title "
                            "pattern. Double-click an entry to edit it."));
    auto* h = new QHBoxLayout;
    h->setSpacing(14);
    h->addWidget(new ListEditor(true, client, model));
    h->addWidget(new ListEditor(false, client, model));
    v->addLayout(h);
    v->addWidget(ui::hintLabel(tr("If a window matches both lists, the block list wins. Your own clicks and "
                                  "Alt+Tab always switch to any window.")));
    v->addStretch();
}

// ------------------------------------------------------------------ WindowPicker

WindowPicker::WindowPicker(DaemonClient* client, bool allowList, QWidget* parent)
    : QDialog(parent), client_(client), allowList_(allowList) {
    setWindowTitle(tr("Pick a window"));
    resize(760, 460);
    auto* v = new QVBoxLayout(this);
    v->setSpacing(10);
    v->addWidget(ui::hintLabel(tr("Choose one of your open windows, then how it should be recognised.")));

    tree_ = new QTreeWidget;
    tree_->setObjectName("table");
    tree_->setHeaderLabels({tr("App"), tr("Window"), tr("Process"), tr("Class"), tr("Currently")});
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tree_->setColumnWidth(0, 140);
    tree_->setColumnWidth(2, 120);
    tree_->setColumnWidth(3, 120);
    tree_->setColumnWidth(4, 90);
    v->addWidget(tree_, 1);
    offline_ = ui::hintLabel(tr("FocusGuard isn't running, so the open windows can't be listed. Start it on the "
                                "Status page."));
    offline_->setAlignment(Qt::AlignCenter);
    v->addWidget(offline_);

    auto* match = new QHBoxLayout;
    match->addWidget(new QLabel(tr("Recognise it by")));
    matchBy_ = new QComboBox;
    matchBy_->addItems({tr("App name"), tr("Exact window title"), tr("Window title containing it")});
    match->addWidget(matchBy_);
    preview_ = new QLabel;
    preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    match->addWidget(preview_, 1);
    auto* refresh = ui::button(tr("Refresh"));
    match->addWidget(refresh);
    v->addLayout(match);

    auto* buttons = new QDialogButtonBox;
    addAllow_ = buttons->addButton(tr("Add to allow list"), QDialogButtonBox::AcceptRole);
    addBlock_ = buttons->addButton(tr("Add to block list"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    addAllow_->setObjectName(allowList ? "primary" : "secondary");
    addBlock_->setObjectName(allowList ? "secondary" : "primary");
    (allowList ? addAllow_ : addBlock_)->setDefault(true);
    v->addWidget(buttons);

    auto accept = [this](bool allow) {
        entry_ = makeEntry();
        allowList_ = allow;
        if (!entry_.isEmpty()) QDialog::accept();
    };
    connect(addAllow_, &QPushButton::clicked, this, [accept] { accept(true); });
    connect(addBlock_, &QPushButton::clicked, this, [accept] { accept(false); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(refresh, &QPushButton::clicked, this, [this] { client_->send("windows"); });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &WindowPicker::updatePreview);
    connect(matchBy_, qOverload<int>(&QComboBox::currentIndexChanged), this, &WindowPicker::updatePreview);
    connect(client_, &DaemonClient::windowsReceived, this, &WindowPicker::fill);

    const bool online = client_->isConnected();
    tree_->setVisible(online);
    offline_->setVisible(!online);
    refresh->setEnabled(online);
    if (online) client_->send("windows");
    updatePreview();
}

void WindowPicker::fill(const QJsonArray& windows) {
    tree_->clear();
    const ui::Theme t = ui::Theme::current();
    for (const QJsonValue& v : windows) {
        const QJsonObject w = v.toObject();
        const bool allowed = w.value("allowed").toBool(), blocked = w.value("blocked").toBool();
        auto* item = new QTreeWidgetItem({w.value("app").toString(), w.value("title").toString(),
                                          w.value("process").toString(), w.value("class").toString(),
                                          blocked ? tr("Blocked") : allowed ? tr("Allowed") : QString()});
        item->setData(0, Qt::UserRole, w);
        item->setToolTip(1, w.value("title").toString());
        if (blocked || allowed) item->setForeground(4, blocked ? t.bad : t.good);
        tree_->addTopLevelItem(item);
    }
    if (tree_->topLevelItemCount()) tree_->setCurrentItem(tree_->topLevelItem(0));
}

QString WindowPicker::makeEntry() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (!item) return {};
    const QJsonObject w = item->data(0, Qt::UserRole).toJsonObject();
    const QString title = w.value("title").toString().simplified().remove(',');  // commas split entries
    switch (matchBy_->currentIndex()) {
        case 1: return title.isEmpty() ? QString() : "title:" + title;
        case 2: return title.isEmpty() ? QString() : "title:*" + title + "*";
        default: return appEntry(w);
    }
}

void WindowPicker::updatePreview() {
    const QString e = makeEntry();
    preview_->setText(e.isEmpty() ? QString() : tr("Entry: <b>%1</b>").arg(e.toHtmlEscaped()));
    addAllow_->setEnabled(!e.isEmpty());
    addBlock_->setEnabled(!e.isEmpty());
}
