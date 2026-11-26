#pragma once

namespace infalsus::gui {

inline constexpr bool kDefaultInvertMousewheelScroll = false;
inline constexpr bool kDefaultConvertNoteHoldOnResize = false;

struct HandlingSettings {
    bool invertMousewheelScroll = kDefaultInvertMousewheelScroll;
    bool convertNoteHoldOnResize = kDefaultConvertNoteHoldOnResize;
};

} // namespace infalsus::gui
