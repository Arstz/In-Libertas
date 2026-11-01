#pragma once

#include <QtWidgets/QWidget>

namespace infalsus::gui {

[[nodiscard]] inline int splitterGripY(const QWidget* widget, const QRect& area) {
    for (const QWidget* reference = widget; reference != nullptr; reference = reference->parentWidget()) {
        if (reference->objectName() == QStringLiteral("workspaceSplitter")) {
            const int y = widget->mapFrom(reference, reference->rect().center()).y();
            if (y >= area.top() + 6 && y <= area.bottom() - 6) {
                return y;
            }
            break;
        }
    }
    return area.center().y();
}

} // namespace infalsus::gui
