// SPDX-License-Identifier: AGPL-3.0-or-later
#include "widgets.h"

#include <QApplication>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace ui {

// ------------------------------------------------------------------ theme

static QColor mix(const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

Theme Theme::current() {
    const QPalette pal = QApplication::palette();
    Theme t;
    t.window = pal.color(QPalette::Window);
    t.text = pal.color(QPalette::WindowText);
    t.dark = t.window.lightness() < 128;
    t.accent = t.dark ? QColor(0x5b, 0x9b, 0xf0) : QColor(0x23, 0x6b, 0xd0);
    t.accentText = t.dark ? QColor(0x9c, 0xc4, 0xff) : QColor(0x1a, 0x52, 0xa3);
    t.card = t.dark ? t.window.lighter(118) : mix(t.window, Qt::white, 0.75);
    t.sidebar = t.dark ? t.window.darker(118) : t.window.darker(103);
    t.border = t.dark ? t.window.lighter(150) : t.window.darker(114);
    t.hover = mix(t.sidebar, t.text, 0.07);
    t.muted = mix(t.text, t.window, 0.42);
    t.good = t.dark ? QColor(0x4c, 0xc3, 0x7a) : QColor(0x1e, 0x8e, 0x4a);
    t.bad = t.dark ? QColor(0xff, 0x7a, 0x6e) : QColor(0xc6, 0x32, 0x28);
    t.warn = t.dark ? QColor(0xf0, 0xb2, 0x3c) : QColor(0xa8, 0x6a, 0x00);
    return t;
}

QString styleSheet(const Theme& t) {
    auto c = [](const QColor& col) { return col.name(QColor::HexArgb); };
    QColor soft = t.accent;
    soft.setAlphaF(t.dark ? 0.22 : 0.14);
    QColor warnSoft = t.warn;
    warnSoft.setAlphaF(0.14);
    return QStringLiteral(
               "#sidebar { background: %1; border-right: 1px solid %2; }"
               "#brand { font-size: 13pt; font-weight: 600; }"
               "QListWidget#nav { background: transparent; border: none; outline: 0; }"
               "QListWidget#nav::item { padding: 8px 10px; border-radius: 8px; margin: 1px 10px; color: %3; }"
               "QListWidget#nav::item:hover { background: %4; }"
               "QListWidget#nav::item:selected { background: %5; color: %6; }"
               "#card { background: %7; border: 1px solid %2; border-radius: 12px; }"
               "#cardTitle { font-weight: 600; font-size: 11pt; }"
               "#pageTitle { font-size: 18pt; font-weight: 600; }"
               "#heroTitle { font-size: 15pt; font-weight: 600; }"
               "#bigNumber { font-size: 26pt; font-weight: 600; }"
               "#hint { color: %8; }"
               "#saveBar { background: %7; border-top: 1px solid %2; }"
               "#banner { background: %9; border: 1px solid %10; border-radius: 8px; padding: 8px 10px; }"
               "QFrame#banner > QLabel { background: transparent; border: none; padding: 0; }"
               "QPushButton#primary { background: %11; color: white; border: 1px solid %11; border-radius: 6px;"
               " padding: 6px 16px; font-weight: 600; }"
               "QPushButton#primary:hover { background: %12; border-color: %12; }"
               "QPushButton#primary:pressed { background: %13; border-color: %13; }"
               "QPushButton#primary:disabled { background: %8; border-color: %8; }"
               "QPushButton#secondary { background: %7; color: %3; border: 1px solid %2; border-radius: 6px;"
               " padding: 6px 14px; }"
               "QPushButton#secondary:hover { background: %4; }"
               "QPushButton#secondary:pressed { background: %5; }"
               "QPushButton#secondary:disabled { color: %8; }"
               "QListWidget#entries, QTreeWidget#table { border: none; background: transparent; outline: 0; }"
               "QListWidget#entries { border: 1px solid %2; border-radius: 8px; padding: 4px; }"
               "QListWidget#entries::item { padding: 6px 8px; border-radius: 6px; color: %3; }"
               "QTreeWidget#table::item { padding: 5px 2px; }"
               "QListWidget#entries::item:hover, QTreeWidget#table::item:hover { background: %4; }"
               "QListWidget#entries::item:selected, QTreeWidget#table::item:selected { background: %5; color: %3; }")
        .arg(c(t.sidebar), c(t.border), c(t.text), c(t.hover), c(soft), c(t.accentText), c(t.card), c(t.muted),
             c(warnSoft))
        .arg(c(t.warn), c(t.accent), c(t.accent.lighter(112)), c(t.accent.darker(115)));
}

// ------------------------------------------------------------------ painting

// Half-width of the shield at height y (both in [-1, 1], y down); the same
// outline as the Linux tray icon (src/tray_sni.hpp).
static double shieldHalfWidth(double y) {
    constexpr double top = -0.86, waist = 0.0, bottom = 0.94, w = 0.74, r = 0.2;
    if (y < top || y > bottom) return 0;
    if (y < top + r) {
        const double dy = top + r - y;
        return w - r + std::sqrt(std::max(0.0, r * r - dy * dy));
    }
    if (y <= waist) return w;
    const double t = (y - waist) / (bottom - waist);
    return w * (1 - t * t);
}

static QPainterPath shieldPath(const QRectF& r, double k) {
    constexpr double top = -0.86, bottom = 0.94, cy = 0.04;
    constexpr int kSteps = 64;
    auto map = [&](double x, double y) {
        return QPointF(r.center().x() + x * k * r.width() / 2, r.center().y() + (cy + (y - cy) * k) * r.height() / 2);
    };
    QPainterPath path;
    for (int i = 0; i <= kSteps; ++i) {
        const double y = top + (bottom - top) * i / kSteps;
        i == 0 ? path.moveTo(map(shieldHalfWidth(y), y)) : path.lineTo(map(shieldHalfWidth(y), y));
    }
    for (int i = kSteps; i >= 0; --i) {
        const double y = top + (bottom - top) * i / kSteps;
        path.lineTo(map(-shieldHalfWidth(y), y));
    }
    path.closeSubpath();
    return path;
}

void paintShield(QPainter& p, const QRectF& r, ShieldState st) {
    QColor border(0x1c, 0x4f, 0x8a), light(0x4a, 0x99, 0xf0), dark(0x2e, 0x73, 0xd1);
    if (st == ShieldState::Paused || st == ShieldState::Off) {
        border = QColor(0x55, 0x55, 0x55), light = QColor(0xa8, 0xa8, 0xa8), dark = QColor(0x8a, 0x8a, 0x8a);
        if (st == ShieldState::Off) border = QColor(0x80, 0x80, 0x80), light = dark = QColor(0xc4, 0xc4, 0xc4);
    } else if (st == ShieldState::DryRun) {
        border = QColor(0x8c, 0x5c, 0x00), light = QColor(0xff, 0xc4, 0x40), dark = QColor(0xe8, 0xa3, 0x1a);
    }
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(border);
    p.drawPath(shieldPath(r, 1.0));
    QLinearGradient halves(r.left(), 0, r.right(), 0);
    halves.setColorAt(0.0, light);
    halves.setColorAt(0.4999, light);
    halves.setColorAt(0.5, dark);
    halves.setColorAt(1.0, dark);
    p.setBrush(halves);
    p.drawPath(shieldPath(r, 0.8));

    const double u = r.width() / 2;  // one unit of the [-1, 1] icon space
    auto at = [&](double x, double y) { return QPointF(r.center().x() + x * u, r.center().y() + y * r.height() / 2); };
    if (st == ShieldState::Active) {
        QPen pen(Qt::white, 0.2 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath check;
        check.moveTo(at(-0.36, 0.02));
        check.lineTo(at(-0.1, 0.3));
        check.lineTo(at(0.36, -0.3));
        p.drawPath(check);
    } else if (st == ShieldState::Paused) {
        p.setBrush(Qt::white);
        for (double x : {-0.29, 0.09}) p.drawRect(QRectF(at(x, -0.38), at(x + 0.2, 0.34)));
    } else if (st == ShieldState::Off) {
        QPen pen(Qt::white, 0.16 * u, Qt::SolidLine, Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(at(-0.28, 0.0), at(0.28, 0.0));
    }
    p.restore();
}

QIcon appIcon() {
    QIcon icon;
    for (int s : {16, 20, 24, 32, 40, 48, 64, 128, 256}) {
        QPixmap pm(s, s);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paintShield(p, QRectF(0, 0, s, s), ShieldState::Active);
        icon.addPixmap(pm);
    }
    return icon;
}

// Simple line icons, drawn at 2x for sharp results on high-DPI screens.
static QPixmap navPixmap(const QString& name, const QColor& color) {
    constexpr int kSize = 20, kScale = 2;
    QPixmap pm(kSize * kScale, kSize * kScale);
    pm.setDevicePixelRatio(kScale);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    if (name == "status") {
        p.drawPath(shieldPath(QRectF(2.5, 1.5, 15, 17), 1.0));
    } else if (name == "activity") {
        const QPointF pts[] = {{2, 11}, {6, 11}, {8.5, 5}, {11.5, 16}, {14, 9}, {18, 9}};
        p.drawPolyline(pts, 6);
    } else if (name == "settings") {
        for (int i = 0; i < 3; ++i) {
            const double y = 5 + i * 5;
            const double knob = i == 1 ? 13 : 7;
            p.drawLine(QPointF(3, y), QPointF(17, y));
            p.setBrush(color);
            p.drawEllipse(QPointF(knob, y), 2.2, 2.2);
            p.setBrush(Qt::NoBrush);
        }
    } else if (name == "lists") {
        for (int i = 0; i < 3; ++i) {
            const double y = 5 + i * 5;
            p.setBrush(color);
            p.drawEllipse(QPointF(4, y), 1.2, 1.2);
            p.setBrush(Qt::NoBrush);
            p.drawLine(QPointF(8, y), QPointF(17, y));
        }
    }
    return pm;
}

QIcon navIcon(const QString& name, const Theme& t) {
    QIcon icon;
    icon.addPixmap(navPixmap(name, t.muted), QIcon::Normal);
    icon.addPixmap(navPixmap(name, t.accentText), QIcon::Selected);
    return icon;
}

// ------------------------------------------------------------------ Switch

Switch::Switch(QWidget* parent) : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    anim_.setDuration(130);
    anim_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        pos_ = v.toReal();
        update();
    });
    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        anim_.stop();
        if (!isVisible()) {  // programmatic changes while hidden: no animation
            pos_ = on ? 1 : 0;
            update();
            return;
        }
        anim_.setStartValue(pos_);
        anim_.setEndValue(on ? 1.0 : 0.0);
        anim_.start();
    });
}

void Switch::paintEvent(QPaintEvent*) {
    const Theme t = Theme::current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled()) p.setOpacity(0.45);
    const QRectF track = QRectF(rect()).adjusted(1, 1, -1, -1);
    const qreal radius = track.height() / 2;
    const QColor off = t.dark ? QColor(0x5c, 0x5c, 0x5c) : QColor(0xbd, 0xbd, 0xbd);
    QColor fill = mix(off, t.accent, pos_);
    if (hover_ && isEnabled()) fill = fill.lighter(108);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(track, radius, radius);
    if (hasFocus()) {
        QPen ring(t.accent, 1.5);
        p.setPen(ring);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(track.adjusted(-0.5, -0.5, 0.5, 0.5), radius, radius);
        p.setPen(Qt::NoPen);
    }
    const qreal d = track.height() - 6;
    const qreal x = track.left() + 3 + (track.width() - 6 - d) * pos_;
    p.setBrush(QColor(0, 0, 0, 40));
    p.drawEllipse(QRectF(x, track.top() + 3.6, d, d));  // soft shadow
    p.setBrush(Qt::white);
    p.drawEllipse(QRectF(x, track.top() + 3, d, d));
}

void Switch::enterEvent(QEnterEvent* e) {
    hover_ = true;
    update();
    QAbstractButton::enterEvent(e);
}

void Switch::leaveEvent(QEvent* e) {
    hover_ = false;
    update();
    QAbstractButton::leaveEvent(e);
}

// ------------------------------------------------------------------ layout helpers

QLabel* hintLabel(const QString& text) {
    auto* l = new QLabel(text);
    l->setObjectName("hint");
    l->setWordWrap(true);
    return l;
}

QPushButton* button(const QString& text, bool primary) {
    auto* b = new QPushButton(text);
    b->setObjectName(primary ? "primary" : "secondary");
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

QLabel* pageTitle(const QString& text) {
    auto* l = new QLabel(text);
    l->setObjectName("pageTitle");
    return l;
}

Card::Card(const QString& title, const QString& subtitle, QWidget* parent) : QFrame(parent) {
    setObjectName("card");
    body_ = new QVBoxLayout(this);
    body_->setContentsMargins(18, 14, 18, 16);
    body_->setSpacing(12);
    if (!title.isEmpty()) {
        auto* head = new QVBoxLayout;
        head->setSpacing(2);
        auto* t = new QLabel(title);
        t->setObjectName("cardTitle");
        head->addWidget(t);
        if (!subtitle.isEmpty()) head->addWidget(hintLabel(subtitle));
        body_->addLayout(head);
    }
}

QWidget* settingRow(const QString& title, const QString& hint, QWidget* control) {
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(16);
    auto* text = new QVBoxLayout;
    text->setSpacing(1);
    auto* t = new QLabel(title);
    t->setWordWrap(true);
    text->addWidget(t);
    if (!hint.isEmpty()) text->addWidget(hintLabel(hint));
    h->addLayout(text, 1);
    h->addWidget(control, 0, Qt::AlignVCenter);
    return row;
}

// ------------------------------------------------------------------ ShieldWidget

ShieldWidget::ShieldWidget(int size, QWidget* parent) : QWidget(parent) { setFixedSize(size, size); }

void ShieldWidget::setState(ShieldState s) {
    state_ = s;
    update();
}

void ShieldWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    paintShield(p, QRectF(rect()), state_);
}

// ------------------------------------------------------------------ BarList

BarList::BarList(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void BarList::setItems(const QVector<QPair<QString, int>>& items) {
    items_ = items;
    updateGeometry();
    update();
}

QSize BarList::sizeHint() const { return {240, std::max<int>(1, (int)items_.size()) * 28}; }

void BarList::paintEvent(QPaintEvent*) {
    if (items_.isEmpty()) return;
    const Theme t = Theme::current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    int max = 1;
    for (const auto& it : items_) max = std::max(max, it.second);
    const int w = width(), nameW = w * 38 / 100, countW = 44;
    const int barX = nameW + 10, barW = std::max(10, w - barX - countW - 8);
    QColor bar = t.accent;
    bar.setAlphaF(0.75);
    QColor track = t.text;
    track.setAlphaF(0.07);
    for (int i = 0; i < (int)items_.size(); ++i) {
        const int y = i * 28;
        const QRect nameRect(0, y, nameW, 28);
        p.setPen(t.text);
        p.drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft,
                   fontMetrics().elidedText(items_[i].first, Qt::ElideRight, nameW));
        p.setPen(Qt::NoPen);
        p.setBrush(track);
        p.drawRoundedRect(QRectF(barX, y + 10, barW, 8), 4, 4);
        p.setBrush(bar);
        p.drawRoundedRect(QRectF(barX, y + 10, std::max(8.0, barW * (double)items_[i].second / max), 8), 4, 4);
        p.setPen(t.muted);
        p.drawText(QRect(w - countW, y, countW, 28), Qt::AlignVCenter | Qt::AlignRight,
                   QString::number(items_[i].second));
    }
}

}  // namespace ui
