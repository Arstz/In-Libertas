#pragma once

#include <QtCore/Qt>

#include <array>

namespace infalsus::gui {

inline constexpr std::array<Qt::Key, 6> kDefaultPlaytestLaneKeys{
    Qt::Key_Shift, Qt::Key_A, Qt::Key_S, Qt::Key_D, Qt::Key_F, Qt::Key_Space};
inline constexpr double kDefaultPlaytestMouseSensitivity = 1.0;
inline constexpr double kMinimumPlaytestMouseSensitivity = 0.1;
inline constexpr double kMaximumPlaytestMouseSensitivity = 10.0;

struct PlaytestSettings {
    std::array<Qt::Key, 6> laneKeys = kDefaultPlaytestLaneKeys;
    double mouseSensitivity = kDefaultPlaytestMouseSensitivity;
};

} // namespace infalsus::gui
