#pragma once

#include "gui/key_bindings.h"
#include "gui/app/visual_settings.h"
#include "gui/app/handling_settings.h"

#include <QtWidgets/QDialog>

class QKeySequenceEdit;
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QSettings;
class QStackedWidget;

namespace infalsus::gui {

class SettingsDialog final : public QDialog {
public:
    explicit SettingsDialog(KeyBindingRouter* keyBindings, const VisualSettings& visuals,
        const HandlingSettings& handling, QWidget* parent = nullptr);

    [[nodiscard]] VisualSettings selectedVisualSettings() const;
    [[nodiscard]] static VisualSettings loadVisualSettings();
    [[nodiscard]] HandlingSettings selectedHandlingSettings() const;
    [[nodiscard]] static HandlingSettings loadHandlingSettings();

    static void restoreKeybinds(KeyBindingRouter* keyBindings);
    static void restoreKeybinds(KeyBindingRouter* keyBindings, QSettings& settings);

protected:
    void accept() override;

private:
    void buildVisualsPage(QStackedWidget* pages);
    void choosePlayheadColor();
    void updateVisualControls();
    void saveVisualSettings(const VisualSettings& visuals);
    void buildHandlingPage(QStackedWidget* pages, const HandlingSettings& handling);
    void saveHandlingSettings(const HandlingSettings& handling);
    [[nodiscard]] QVector<KeyBinding> editedBindings() const;
    bool updateConflicts();
    void resetShortcut(int row);
    void resetAllShortcuts();
    void saveKeybinds(const QVector<KeyBinding>& bindings);

    KeyBindingRouter* m_keyBindings = nullptr;
    VisualSettings m_visuals;
    QComboBox* m_playheadColorMode = nullptr;
    QPushButton* m_playheadColorButton = nullptr;
    QCheckBox* m_invertMousewheelScroll = nullptr;
    QVector<KeyBinding> m_bindings;
    QVector<QKeySequenceEdit*> m_shortcutEdits;
    QLabel* m_validationLabel = nullptr;
};

} // namespace infalsus::gui
