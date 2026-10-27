// SPDX-License-Identifier: AGPL-3.0-or-later
// FocusGuard Settings - look and feel: theme colours, the stylesheet, painted
// icons and a few small custom widgets.
#pragma once

#include <QAbstractButton>
#include <QFrame>
#include <QIcon>
#include <QVariantAnimation>
#include <QVector>

class QLabel;
class QPainter;
class QPushButton;
class QVBoxLayout;

namespace ui {

// Colours derived from the current palette, so light and dark themes both work.
struct Theme {
    bool dark = false;
    QColor window, card, sidebar, border, hover, text, muted, accent, accentText, good, bad, warn;
    static Theme current();
};

QString styleSheet(const Theme& t);

enum class ShieldState { Active, Paused, DryRun, Off };
void paintShield(QPainter& p, const QRectF& r, ShieldState st);
QIcon appIcon();
// Sidebar icons: "status", "activity", "settings", "lists".
QIcon navIcon(const QString& name, const Theme& t);

// An animated on/off switch.
class Switch : public QAbstractButton {
    Q_OBJECT
public:
    explicit Switch(QWidget* parent = nullptr);
    QSize sizeHint() const override { return {42, 24}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    qreal pos_ = 0;  // knob position: 0 = off, 1 = on
    bool hover_ = false;
    QVariantAnimation anim_;
};

// A rounded panel with an optional title and subtitle; add content to body().
class Card : public QFrame {
public:
    explicit Card(const QString& title = {}, const QString& subtitle = {}, QWidget* parent = nullptr);
    QVBoxLayout* body() const { return body_; }

private:
    QVBoxLayout* body_;
};

// Title and explanation on the left, a control on the right.
QWidget* settingRow(const QString& title, const QString& hint, QWidget* control);
QLabel* hintLabel(const QString& text);
// A push button in the app's style; `primary` for the main action of a view.
QPushButton* button(const QString& text, bool primary = false);
QLabel* pageTitle(const QString& text);

class ShieldWidget : public QWidget {
public:
    explicit ShieldWidget(int size, QWidget* parent = nullptr);
    void setState(ShieldState s);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    ShieldState state_ = ShieldState::Off;
};

// Horizontal bars: name, proportional bar, count.
class BarList : public QWidget {
public:
    explicit BarList(QWidget* parent = nullptr);
    void setItems(const QVector<QPair<QString, int>>& items);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QVector<QPair<QString, int>> items_;
};

}  // namespace ui
