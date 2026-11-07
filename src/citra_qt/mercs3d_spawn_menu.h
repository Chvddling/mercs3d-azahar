// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <QDialog>
#include <QPointer>
#include "core/mercs3d_spawn.h"

class QCheckBox;
class QLabel;
class QPushButton;

/// Top-down sketch of the starting points (north up, like the minimap).
class Mercs3DSpawnMap : public QWidget {
    Q_OBJECT

public:
    explicit Mercs3DSpawnMap(QWidget* parent = nullptr);
    void SetPoints(std::vector<Mercs3D::SpawnPoint> points);
    void SetSelected(int index); ///< -1 = random

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<Mercs3D::SpawnPoint> points;
    int selected = 0;
};

/// "Choose your starting point" window shown while a mission loads.
class Mercs3DSpawnDialog : public QDialog {
    Q_OBJECT

public:
    Mercs3DSpawnDialog(const Mercs3D::SpawnRequest& request, QWidget* parent);
    void HandleInput(Mercs3D::MenuInput input);

protected:
    void reject() override; ///< Closing the window = random.

private:
    void Select(int option); ///< 0..count-1 = point, count = random
    void Finish();

    std::vector<Mercs3D::SpawnPoint> points;
    std::vector<QPushButton*> options;
    Mercs3DSpawnMap* map;
    QLabel* details;
    QCheckBox* remember;
    int selected = 0;
    bool finished = false;
};

/// Connects the core's starting point menu to Qt (calls come from the emulation thread).
class Mercs3DSpawnMenu : public QObject, public Mercs3D::SpawnMenuFrontend {
    Q_OBJECT

public:
    explicit Mercs3DSpawnMenu(QWidget* parent_window);
    ~Mercs3DSpawnMenu() override;

    void OpenSpawnMenu(const Mercs3D::SpawnRequest& request) override;
    void SpawnMenuInput(Mercs3D::MenuInput input) override;

    /// Closes an open menu and releases the emulation thread. Call before stopping a game.
    void Cancel();

private:
    QWidget* parent_window;
    QPointer<Mercs3DSpawnDialog> dialog;
};
