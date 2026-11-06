// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <QWidget>
#include "core/mercs3d_layout.h"

class QCheckBox;
class QSpinBox;

/// Settings tab for the Resident Evil: The Mercenaries 3D single screen layout.
/// Changes show up in the game right away; OK saves them, Cancel undoes them.
class ConfigureMercs3D : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureMercs3D(QWidget* parent = nullptr);
    ~ConfigureMercs3D() override;

    void SetConfiguration();
    void ApplyConfiguration();

private:
    struct OverlayControls {
        QCheckBox* enabled;
        QSpinBox* x;
        QSpinBox* y;
        QSpinBox* size;
        QSpinBox* alpha;
    };

    QWidget* MakeOverlayGroup(const QString& title, OverlayControls& controls);
    void Load(const Mercs3D::LayoutSettings& settings);
    Mercs3D::LayoutSettings Read() const;
    void OnChanged();

    QCheckBox* single_screen;
    QCheckBox* widescreen;
    OverlayControls minimap{};
    OverlayControls items{};

    Mercs3D::LayoutSettings original;
    bool applied = false;
    bool loading = false;
};
