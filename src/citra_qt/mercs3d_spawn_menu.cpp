// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <QCheckBox>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include "citra_qt/mercs3d_spawn_menu.h"

namespace {

/// Compass name of a direction on the map (world +X = east, +Z = south).
QString Compass(float dx, float dz) {
    if (std::hypot(dx, dz) < 1.0f) {
        return QObject::tr("centre");
    }
    static const char* names[] = {
        QT_TRANSLATE_NOOP("Mercs3DSpawnDialog", "east"),
        QT_TRANSLATE_NOOP("Mercs3DSpawnDialog", "north-east"),
        QT_TRANSLATE_NOOP("Mercs3DSpawnDialog", "north"),
        QT_TRANSLATE_NOOP("Mercs3DSpawnDialog", "north-west"),
        QT_TRANSLATE_NOOP("Mercs3DSpawnDialog", "west"),
        QT_TRANSLATE_NOOP("Mercs3DSpawnDialog", "south-west"),
        QT_TRANSLATE_NOOP("Mercs3DSpawnDialog", "south"),
        QT_TRANSLATE_NOOP("Mercs3DSpawnDialog", "south-east"),
    };
    const float angle = std::atan2(-dz, dx); // counter-clockwise from east, north up
    const int sector = static_cast<int>(std::lround(angle / (3.14159265f / 4.0f)) + 8) % 8;
    return QCoreApplication::translate("Mercs3DSpawnDialog", names[sector]);
}

float LowestY(const std::vector<Mercs3D::SpawnPoint>& points) {
    float low = points.front().y;
    for (const auto& p : points) {
        low = std::min(low, p.y);
    }
    return low;
}

} // namespace

Mercs3DSpawnMap::Mercs3DSpawnMap(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
}

void Mercs3DSpawnMap::SetPoints(std::vector<Mercs3D::SpawnPoint> points_) {
    points = std::move(points_);
    update();
}

void Mercs3DSpawnMap::SetSelected(int index) {
    selected = index;
    update();
}

void Mercs3DSpawnMap::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(24, 24, 24));
    if (points.empty()) {
        return;
    }

    float min_x = points.front().x, max_x = min_x, min_z = points.front().z, max_z = min_z;
    for (const auto& pt : points) {
        min_x = std::min(min_x, pt.x);
        max_x = std::max(max_x, pt.x);
        min_z = std::min(min_z, pt.z);
        max_z = std::max(max_z, pt.z);
    }
    // At least 20 m across, so close points don't sit on the edges.
    const float span = std::max({max_x - min_x, max_z - min_z, 2000.0f});
    const float cx = (min_x + max_x) / 2.0f, cz = (min_z + max_z) / 2.0f;
    const float margin = 40.0f;
    const float scale = std::min(width() - 2 * margin, height() - 2 * margin) / span;
    auto to_screen = [&](const Mercs3D::SpawnPoint& pt) {
        return QPointF(width() / 2.0 + (pt.x - cx) * scale, height() / 2.0 + (pt.z - cz) * scale);
    };

    // 10 m grid.
    p.setPen(QColor(48, 48, 48));
    const float step = 1000.0f * scale;
    if (step > 8.0f) {
        for (float x = std::fmod(width() / 2.0f, step); x < width(); x += step) {
            p.drawLine(QPointF(x, 0), QPointF(x, height()));
        }
        for (float y = std::fmod(height() / 2.0f, step); y < height(); y += step) {
            p.drawLine(QPointF(0, y), QPointF(width(), y));
        }
    }
    p.setPen(QColor(150, 150, 150));
    p.drawText(QRectF(0, 4, width(), 20), Qt::AlignHCenter, tr("N"));
    p.drawText(QRectF(8, height() - 22, width() - 16, 20), Qt::AlignRight, tr("grid: 10 m"));

    const float low = LowestY(points);
    QFont font = p.font();
    font.setBold(true);
    p.setFont(font);
    for (std::size_t i = 0; i < points.size(); i++) {
        const QPointF c = to_screen(points[i]);
        const bool on = selected < 0 || static_cast<int>(i) == selected;
        const QColor color = on ? QColor(220, 40, 40) : QColor(110, 60, 60);
        p.setPen(QPen(on ? Qt::white : QColor(90, 90, 90), 2));
        p.setBrush(color);
        const double radius = static_cast<int>(i) == selected ? 16 : 12;
        p.drawEllipse(c, radius, radius);
        p.setPen(Qt::white);
        p.drawText(QRectF(c.x() - 16, c.y() - 16, 32, 32), Qt::AlignCenter,
                   QString::number(i + 1));
        const float up = (points[i].y - low) / 100.0f;
        if (up >= 1.0f) {
            p.setPen(QColor(200, 200, 200));
            p.drawText(QRectF(c.x() + 18, c.y() - 10, 120, 20), Qt::AlignLeft | Qt::AlignVCenter,
                       tr("+%1 m").arg(up, 0, 'f', 1));
        }
    }
}

Mercs3DSpawnDialog::Mercs3DSpawnDialog(const Mercs3D::SpawnRequest& request, QWidget* parent)
    : QDialog(parent), points(request.points) {
    setWindowTitle(tr("%1 - starting point")
                       .arg(QString::fromStdString(Mercs3D::MissionName(request.mission_id))));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(tr("<b>%1</b>: where do you want to start?")
                                 .arg(QString::fromStdString(
                                     Mercs3D::MissionName(request.mission_id))));
    layout->addWidget(title);

    map = new Mercs3DSpawnMap;
    map->SetPoints(points);
    layout->addWidget(map, 1);

    details = new QLabel;
    details->setWordWrap(true);
    layout->addWidget(details);

    auto* row = new QHBoxLayout;
    for (std::size_t i = 0; i <= points.size(); i++) {
        const bool random = i == points.size();
        auto* button = new QPushButton(random ? tr("Random") : tr("Point %1").arg(i + 1));
        button->setCheckable(true);
        button->setAutoDefault(false);
        connect(button, &QPushButton::clicked, this, [this, i] { Select(static_cast<int>(i)); });
        row->addWidget(button);
        options.push_back(button);
    }
    layout->addLayout(row);

    remember = new QCheckBox(tr("Always use this for this mission (Y)"));
    layout->addWidget(remember);

    auto* bottom = new QHBoxLayout;
    auto* hint = new QLabel(tr("Left / Right: choose   A: start   Y: remember"));
    hint->setStyleSheet(QStringLiteral("color: gray"));
    auto* start = new QPushButton(tr("Start (A)"));
    start->setDefault(true);
    connect(start, &QPushButton::clicked, this, &Mercs3DSpawnDialog::Finish);
    bottom->addWidget(hint, 1);
    bottom->addWidget(start);
    layout->addLayout(bottom);

    resize(460, 460);
    Select(0);
}

void Mercs3DSpawnDialog::Select(int option) {
    const int count = static_cast<int>(points.size());
    selected = std::clamp(option, 0, count);
    for (int i = 0; i <= count; i++) {
        options[i]->setChecked(i == selected);
    }
    map->SetSelected(selected == count ? -1 : selected);

    if (selected == count) {
        details->setText(tr("Random: the game picks, as usual."));
        return;
    }
    // Describe the point against the middle of all points.
    float mx = 0, mz = 0;
    for (const auto& p : points) {
        mx += p.x / count;
        mz += p.z / count;
    }
    const auto& p = points[selected];
    const float up = (p.y - LowestY(points)) / 100.0f;
    QString text = tr("Point %1: %2 side").arg(selected + 1).arg(Compass(p.x - mx, p.z - mz));
    text += up >= 1.0f ? tr(", %1 m higher than the lowest point.").arg(up, 0, 'f', 1)
                       : tr(", lowest level.");
    details->setText(text);
}

void Mercs3DSpawnDialog::HandleInput(Mercs3D::MenuInput input) {
    const int count = static_cast<int>(points.size());
    switch (input) {
    case Mercs3D::MenuInput::Previous:
        Select((selected + count) % (count + 1));
        break;
    case Mercs3D::MenuInput::Next:
        Select((selected + 1) % (count + 1));
        break;
    case Mercs3D::MenuInput::ToggleRemember:
        remember->toggle();
        break;
    case Mercs3D::MenuInput::Confirm:
        Finish();
        break;
    }
}

void Mercs3DSpawnDialog::Finish() {
    if (finished) {
        return;
    }
    finished = true;
    const int count = static_cast<int>(points.size());
    Mercs3D::AnswerSpawnMenu({selected == count ? -1 : selected, remember->isChecked()});
    accept();
}

void Mercs3DSpawnDialog::reject() {
    if (!finished) {
        finished = true;
        Mercs3D::AnswerSpawnMenu({-1, false});
    }
    QDialog::reject();
}

Mercs3DSpawnMenu::Mercs3DSpawnMenu(QWidget* parent_window_)
    : QObject(parent_window_), parent_window(parent_window_) {
    Mercs3D::SetSpawnMenuFrontend(this);
}

Mercs3DSpawnMenu::~Mercs3DSpawnMenu() {
    Mercs3D::SetSpawnMenuFrontend(nullptr);
}

void Mercs3DSpawnMenu::OpenSpawnMenu(const Mercs3D::SpawnRequest& request) {
    QMetaObject::invokeMethod(
        this,
        [this, request] {
            if (dialog) {
                dialog->deleteLater();
            }
            dialog = new Mercs3DSpawnDialog(request, parent_window);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowModality(Qt::WindowModal);
            dialog->show();
            dialog->raise();
            dialog->activateWindow();
        },
        Qt::QueuedConnection);
}

void Mercs3DSpawnMenu::SpawnMenuInput(Mercs3D::MenuInput input) {
    QMetaObject::invokeMethod(
        this,
        [this, input] {
            if (dialog) {
                dialog->HandleInput(input);
            }
        },
        Qt::QueuedConnection);
}

void Mercs3DSpawnMenu::Cancel() {
    Mercs3D::CancelSpawnMenu();
    if (dialog) {
        dialog->close();
    }
}
