#pragma once

#include "core/field_presets.h"

#include <QtWidgets/QWidget>

class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace infalsus::gui {

class EditorState;

class FieldPresetsWidget final : public QWidget {
    Q_OBJECT

public:
    explicit FieldPresetsWidget(EditorState* state, QWidget* parent = nullptr);

private:
    void refreshTiles();
    void updateSelection();
    void addSelection();
    void placePreset(int index);
    void loadPresets();
    void savePresets() const;
    [[nodiscard]] qint64 durationMilliseconds() const;

    EditorState* m_state = nullptr;
    QVector<FieldPreset> m_presets;
    QListWidget* m_tiles = nullptr;
    QPushButton* m_addSelection = nullptr;
    QComboBox* m_sizeMode = nullptr;
    QLineEdit* m_milliseconds = nullptr;
    QComboBox* m_divisor = nullptr;
};

}
