#pragma once

#include "gui/state/editor_state.h"

#include <QtWidgets/QDialog>

class QComboBox;
class QLineEdit;

namespace infalsus::gui {

class SvInterpolationDialog final : public QDialog {
public:
    explicit SvInterpolationDialog(int divisor, QWidget* parent = nullptr);

    [[nodiscard]] SpeedInterpolationOptions options() const;

private:
    QComboBox* m_spacing = nullptr;
    QLineEdit* m_milliseconds = nullptr;
    QComboBox* m_divisor = nullptr;
    QComboBox* m_easing = nullptr;
};

} // namespace infalsus::gui
