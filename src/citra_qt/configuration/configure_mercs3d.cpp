// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cmath>
#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include "citra_qt/configuration/configure_mercs3d.h"

namespace {

/// A slider and a spin box showing the same percentage.
QSpinBox* AddPercentRow(QGridLayout* grid, int row, const QString& label, int min, int max) {
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(min, max);
    auto* spin = new QSpinBox;
    spin->setRange(min, max);
    spin->setSuffix(QStringLiteral(" %"));
    QObject::connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
    QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), slider, &QSlider::setValue);
    grid->addWidget(new QLabel(label), row, 0);
    grid->addWidget(slider, row, 1);
    grid->addWidget(spin, row, 2);
    return spin;
}

int ToPercent(float value) {
    return static_cast<int>(std::lround(value * 100.0f));
}

float FromPercent(int value) {
    return static_cast<float>(value) / 100.0f;
}

} // namespace

ConfigureMercs3D::ConfigureMercs3D(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(
        tr("Resident Evil: The Mercenaries 3D. During a match only the top screen is shown, with "
           "the minimap and item grid from the bottom screen placed on top of it. Menus and the "
           "pause menu use both screens; the results screen uses the top screen only.\n"
           "Positions and sizes are percentages of the game picture. Changes show right away."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    single_screen = new QCheckBox(tr("Single screen layout during matches"));
    widescreen = new QCheckBox(tr("Keep the game picture at 16:9 (black bars on other windows)"));
    layout->addWidget(single_screen);
    layout->addWidget(widescreen);
    layout->addWidget(MakeOverlayGroup(tr("Minimap"), minimap));
    layout->addWidget(MakeOverlayGroup(tr("Item grid"), items));

    auto* reset = new QPushButton(tr("Restore defaults"));
    auto* reset_row = new QHBoxLayout;
    reset_row->addStretch();
    reset_row->addWidget(reset);
    layout->addLayout(reset_row);
    layout->addStretch();

    connect(single_screen, &QCheckBox::toggled, this, &ConfigureMercs3D::OnChanged);
    connect(widescreen, &QCheckBox::toggled, this, &ConfigureMercs3D::OnChanged);
    connect(reset, &QPushButton::clicked, this, [this] {
        Load(Mercs3D::LayoutSettings{});
        OnChanged();
    });

    original = Mercs3D::GetLayoutSettings();
    Load(original);
}

ConfigureMercs3D::~ConfigureMercs3D() {
    if (!applied) {
        Mercs3D::SetLayoutSettings(original);
    }
}

QWidget* ConfigureMercs3D::MakeOverlayGroup(const QString& title, OverlayControls& controls) {
    auto* group = new QGroupBox(title);
    auto* grid = new QGridLayout(group);
    controls.enabled = new QCheckBox(tr("Show"));
    grid->addWidget(controls.enabled, 0, 0, 1, 3);
    controls.x = AddPercentRow(grid, 1, tr("Left"), -50, 150);
    controls.y = AddPercentRow(grid, 2, tr("Top"), -50, 150);
    controls.size = AddPercentRow(grid, 3, tr("Size"), 5, 100);
    controls.alpha = AddPercentRow(grid, 4, tr("Opacity"), 0, 100);
    grid->setColumnStretch(1, 1);

    connect(controls.enabled, &QCheckBox::toggled, this, &ConfigureMercs3D::OnChanged);
    for (QSpinBox* spin : {controls.x, controls.y, controls.size, controls.alpha}) {
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, &ConfigureMercs3D::OnChanged);
    }
    return group;
}

void ConfigureMercs3D::Load(const Mercs3D::LayoutSettings& settings) {
    loading = true;
    single_screen->setChecked(settings.single_screen);
    widescreen->setChecked(settings.widescreen);
    auto load_overlay = [](OverlayControls& c, const Mercs3D::Overlay& o) {
        c.enabled->setChecked(o.enabled);
        c.x->setValue(ToPercent(o.x));
        c.y->setValue(ToPercent(o.y));
        c.size->setValue(ToPercent(o.size));
        c.alpha->setValue(ToPercent(o.alpha));
    };
    load_overlay(minimap, settings.minimap);
    load_overlay(items, settings.items);
    loading = false;
}

Mercs3D::LayoutSettings ConfigureMercs3D::Read() const {
    Mercs3D::LayoutSettings s;
    s.single_screen = single_screen->isChecked();
    s.widescreen = widescreen->isChecked();
    auto read_overlay = [](const OverlayControls& c) {
        return Mercs3D::Overlay{c.enabled->isChecked(), FromPercent(c.x->value()),
                                FromPercent(c.y->value()), FromPercent(c.size->value()),
                                FromPercent(c.alpha->value())};
    };
    s.minimap = read_overlay(minimap);
    s.items = read_overlay(items);
    return s;
}

void ConfigureMercs3D::OnChanged() {
    if (!loading) {
        Mercs3D::SetLayoutSettings(Read());
    }
}

void ConfigureMercs3D::SetConfiguration() {
    Load(Mercs3D::GetLayoutSettings());
}

void ConfigureMercs3D::ApplyConfiguration() {
    Mercs3D::SetLayoutSettings(Read());
    Mercs3D::SaveLayoutSettings();
    original = Read();
    applied = true;
}
