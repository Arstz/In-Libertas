#pragma once

#include "gui/key_bindings.h"

#include <QtWidgets/QDialog>

class QKeySequenceEdit;
class QLabel;
class QSettings;

namespace infalsus::gui {

class SettingsDialog final : public QDialog {
public:
    explicit SettingsDialog(KeyBindingRouter* keyBindings, QWidget* parent = nullptr);

    static void restoreKeybinds(KeyBindingRouter* keyBindings);
    static void restoreKeybinds(KeyBindingRouter* keyBindings, QSettings& settings);

protected:
    void accept() override;

private:
    [[nodiscard]] QVector<KeyBinding> editedBindings() const;
    bool updateConflicts();
    void resetShortcut(int row);
    void resetAllShortcuts();
    void saveKeybinds(const QVector<KeyBinding>& bindings);

    KeyBindingRouter* m_keyBindings = nullptr;
    QVector<KeyBinding> m_bindings;
    QVector<QKeySequenceEdit*> m_shortcutEdits;
    QLabel* m_validationLabel = nullptr;
};

} // namespace infalsus::gui
