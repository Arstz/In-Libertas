#pragma once

#include <QtGui/QPalette>

class QApplication;

namespace infalsus::gui {

[[nodiscard]] QPalette darkThemePalette();
void applyDarkTheme(QApplication& application);

} // namespace infalsus::gui
