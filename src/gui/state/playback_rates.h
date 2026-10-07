#pragma once

#include <QtCore/QtGlobal>

#include <array>

namespace infalsus::gui {

inline constexpr std::array<qreal, 4> kPlaybackRates{{1.0, 0.75, 0.5, 0.25}};
inline constexpr qreal kFullPlaybackRate = kPlaybackRates.front();

} // namespace infalsus::gui
