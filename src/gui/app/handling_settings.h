#pragma once

namespace infalsus::gui {

inline constexpr bool kDefaultInvertMousewheelScroll = false;

struct HandlingSettings {
    bool invertMousewheelScroll = kDefaultInvertMousewheelScroll;
};

} // namespace infalsus::gui
