#pragma once

#include "gui/app/game_palette.h"

namespace infalsus::gui {

struct VisualSettings {
    QColor playheadColor = palette::guideGreen;
    bool customPlayheadColor = false;

    [[nodiscard]] QColor effectivePlayheadColor() const {
        return customPlayheadColor && playheadColor.isValid() ? playheadColor : palette::guideGreen;
    }
};

} // namespace infalsus::gui
