#pragma once

#include "gui/app/game_palette.h"

namespace infalsus::gui {

inline constexpr double kDefaultNoteSize = 1.0;
inline constexpr double kMinimumNoteSize = 0.5;
inline constexpr double kMaximumNoteSize = 4.0;

struct VisualSettings {
    QColor playheadColor = palette::guideGreen;
    bool customPlayheadColor = false;
    bool animatedToolbarUnfolding = false;
    bool autohideToolbar = false;
    double noteSize = kDefaultNoteSize;

    [[nodiscard]] QColor effectivePlayheadColor() const {
        return customPlayheadColor && playheadColor.isValid() ? playheadColor : palette::guideGreen;
    }
};

} // namespace infalsus::gui
