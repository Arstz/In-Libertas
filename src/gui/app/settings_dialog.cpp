#include "gui/app/settings_dialog.h"

#include <QtCore/QEvent>
#include <QtCore/QSettings>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QColorDialog>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QKeySequenceEdit>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>

namespace infalsus::gui {

namespace {

constexpr int kNavigationWidth = 150;
constexpr int kSettingsWidth = 800;
constexpr int kSettingsHeight = 650;
constexpr int kControlSpacing = 8;
constexpr int kColorTextLightnessThreshold = 128;
constexpr auto kShortcutConflictStyle = "QLineEdit { border: 2px solid #d9534f; }";

void setShortcutConflictStyle(QKeySequenceEdit* edit, const bool conflict) {
    QLineEdit* field = edit->findChild<QLineEdit*>();
    QWidget* target = field != nullptr ? static_cast<QWidget*>(field) : static_cast<QWidget*>(edit);
    target->setStyleSheet(conflict ? QString::fromLatin1(kShortcutConflictStyle) : QString());
}

class ShortcutSequenceEdit final : public QKeySequenceEdit {
public:
    using QKeySequenceEdit::QKeySequenceEdit;

protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
            auto& keyEvent = *static_cast<QKeyEvent*>(event);
            const QKeyCombination combination = keyBindingCombination(keyEvent);
            if (combination.key() == Qt::Key_Tab) {
                if (event->type() == QEvent::KeyPress) {
                    setKeySequence(QKeySequence(combination));
                }
                event->accept();
                return true;
            }
            if (event->type() == QEvent::KeyPress) {
                QKeyEvent normalizedEvent(QEvent::KeyPress, combination.key(), combination.keyboardModifiers(),
                    keyEvent.text(), keyEvent.isAutoRepeat(), keyEvent.count());

                return QKeySequenceEdit::event(&normalizedEvent);
            }
        }

        return QKeySequenceEdit::event(event);
    }
};

} // namespace

SettingsDialog::SettingsDialog(KeyBindingRouter* keyBindings, const VisualSettings& visuals,
    const HandlingSettings& handling, QWidget* parent)
    : QDialog(parent)
    , m_keyBindings(keyBindings)
    , m_visuals(visuals)
    , m_bindings(keyBindings->bindings()) {
    auto* layout = new QVBoxLayout(this);
    auto* contentLayout = new QHBoxLayout;
    auto* navigation = new QListWidget(this);
    auto* pages = new QStackedWidget(this);
    auto* scrollArea = new QScrollArea(pages);
    auto* page = new QWidget(scrollArea);
    auto* pageLayout = new QVBoxLayout(page);
    auto* description = new QLabel(QStringLiteral("Choose a keybind for each command. Clear a keybind to disable it."), page);
    auto* rows = new QFormLayout;
    auto* resetControls = new QHBoxLayout;
    auto* resetAll = new QPushButton(QStringLiteral("Reset All"), page);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_validationLabel = new QLabel(page);

    setWindowTitle(QStringLiteral("Settings"));
    resize(kSettingsWidth, kSettingsHeight);
    navigation->addItems({QStringLiteral("Keybinds"), QStringLiteral("Visuals"), QStringLiteral("Handling")});
    navigation->setCurrentRow(0);
    navigation->setFixedWidth(kNavigationWidth);
    navigation->setUniformItemSizes(true);
    pages->addWidget(scrollArea);
    scrollArea->setWidgetResizable(true);
    scrollArea->setWidget(page);
    description->setWordWrap(true);
    pageLayout->addWidget(description);
    pageLayout->addLayout(rows);
    rows->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_shortcutEdits.reserve(m_bindings.size());
    for (int row = 0; row < m_bindings.size(); ++row) {
        auto* controls = new QWidget(page);
        auto* controlsLayout = new QHBoxLayout(controls);
        auto* edit = new ShortcutSequenceEdit(m_bindings[row].sequence, controls);
        auto* reset = new QPushButton(QStringLiteral("Reset"), controls);
        edit->setObjectName(m_bindings[row].id);
        edit->setClearButtonEnabled(true);
        controlsLayout->setContentsMargins(0, 0, 0, 0);
        controlsLayout->setSpacing(kControlSpacing);
        controlsLayout->addWidget(edit, 1);
        controlsLayout->addWidget(reset);
        rows->addRow(m_bindings[row].label, controls);
        m_shortcutEdits.append(edit);
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [this] { updateConflicts(); });
        connect(reset, &QPushButton::clicked, this, [this, row] { resetShortcut(row); });
    }
    m_validationLabel->setWordWrap(true);
    m_validationLabel->setStyleSheet(QStringLiteral("color: #d07070;"));
    resetControls->addWidget(m_validationLabel, 1);
    resetControls->addWidget(resetAll);
    pageLayout->addLayout(resetControls);
    pageLayout->addStretch();
    buildVisualsPage(pages);
    buildHandlingPage(pages, handling);
    contentLayout->addWidget(navigation);
    contentLayout->addWidget(pages, 1);
    layout->addLayout(contentLayout, 1);
    layout->addWidget(buttons);
    connect(navigation, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(resetAll, &QPushButton::clicked, this, &SettingsDialog::resetAllShortcuts);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateConflicts();
}

VisualSettings SettingsDialog::selectedVisualSettings() const {
    VisualSettings result = m_visuals;
    result.customPlayheadColor = m_playheadColorMode->currentData().toBool();

    return result;
}

VisualSettings SettingsDialog::loadVisualSettings() {
    QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
    VisualSettings result;
    const QColor color(settings.value(QStringLiteral("visuals/playhead_color")).toString());
    if (color.isValid()) {
        result.playheadColor = color;
        result.customPlayheadColor = settings.value(QStringLiteral("visuals/custom_playhead_color"), false).toBool();
    }

    return result;
}

HandlingSettings SettingsDialog::selectedHandlingSettings() const {
    return {.invertMousewheelScroll = m_invertMousewheelScroll->isChecked()};
}

HandlingSettings SettingsDialog::loadHandlingSettings() {
    QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
    HandlingSettings result;
    result.invertMousewheelScroll = settings.value(QStringLiteral("handling/invert_mousewheel_scroll"),
        result.invertMousewheelScroll).toBool();

    return result;
}

void SettingsDialog::restoreKeybinds(KeyBindingRouter* keyBindings) {
    QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
    restoreKeybinds(keyBindings, settings);
}

void SettingsDialog::restoreKeybinds(KeyBindingRouter* keyBindings, QSettings& settings) {
    QVector<KeyBinding> bindings = keyBindings->bindings();
    settings.beginGroup(QStringLiteral("keybinds"));
    for (KeyBinding& binding : bindings) {
        if (settings.contains(binding.id)) {
            binding.sequence = QKeySequence::fromString(settings.value(binding.id).toString(), QKeySequence::PortableText);
        }
    }
    settings.endGroup();
    keyBindings->setBindings(bindings);
}

void SettingsDialog::accept() {
    if (!updateConflicts()) {
        return;
    }
    const QVector<KeyBinding> bindings = editedBindings();
    m_keyBindings->setBindings(bindings);
    saveKeybinds(bindings);
    saveVisualSettings(selectedVisualSettings());
    saveHandlingSettings(selectedHandlingSettings());
    QDialog::accept();
}

void SettingsDialog::buildVisualsPage(QStackedWidget* pages) {
    auto* scrollArea = new QScrollArea(pages);
    auto* page = new QWidget(scrollArea);
    auto* pageLayout = new QVBoxLayout(page);
    auto* rows = new QFormLayout;
    auto* controls = new QWidget(page);
    auto* controlsLayout = new QHBoxLayout(controls);
    m_playheadColorMode = new QComboBox(controls);
    m_playheadColorButton = new QPushButton(controls);

    pages->addWidget(scrollArea);
    scrollArea->setWidgetResizable(true);
    scrollArea->setWidget(page);
    m_playheadColorMode->addItem(QStringLiteral("Theme default"), false);
    m_playheadColorMode->addItem(QStringLiteral("Custom"), true);
    m_playheadColorMode->setCurrentIndex(m_visuals.customPlayheadColor ? 1 : 0);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(kControlSpacing);
    controlsLayout->addWidget(m_playheadColorMode, 1);
    controlsLayout->addWidget(m_playheadColorButton);
    rows->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    rows->addRow(QStringLiteral("Custom playhead color"), controls);
    pageLayout->addLayout(rows);
    pageLayout->addStretch();
    connect(m_playheadColorMode, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateVisualControls);
    connect(m_playheadColorButton, &QPushButton::clicked, this, &SettingsDialog::choosePlayheadColor);
    updateVisualControls();
}

void SettingsDialog::choosePlayheadColor() {
    const QColor picked = QColorDialog::getColor(m_visuals.playheadColor, this, QStringLiteral("Playhead Color"));
    if (!picked.isValid()) {
        return;
    }
    m_visuals.playheadColor = picked;
    updateVisualControls();
}

void SettingsDialog::updateVisualControls() {
    const VisualSettings visuals = selectedVisualSettings();
    const QColor color = visuals.effectivePlayheadColor();
    m_playheadColorButton->setEnabled(visuals.customPlayheadColor);
    m_playheadColorButton->setText(color.name(QColor::HexRgb).toUpper());
    m_playheadColorButton->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
        .arg(color.name(QColor::HexRgb), color.lightness() < kColorTextLightnessThreshold
            ? QStringLiteral("#ffffff") : QStringLiteral("#202225")));
}

void SettingsDialog::saveVisualSettings(const VisualSettings& visuals) {
    QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
    settings.setValue(QStringLiteral("visuals/custom_playhead_color"), visuals.customPlayheadColor);
    settings.setValue(QStringLiteral("visuals/playhead_color"), visuals.playheadColor.name(QColor::HexRgb));
}

void SettingsDialog::buildHandlingPage(QStackedWidget* pages, const HandlingSettings& handling) {
    auto* scrollArea = new QScrollArea(pages);
    auto* page = new QWidget(scrollArea);
    auto* pageLayout = new QVBoxLayout(page);
    m_invertMousewheelScroll = new QCheckBox(QStringLiteral("Invert scroll direction for mousewheel"), page);

    pages->addWidget(scrollArea);
    scrollArea->setWidgetResizable(true);
    scrollArea->setWidget(page);
    m_invertMousewheelScroll->setChecked(handling.invertMousewheelScroll);
    m_invertMousewheelScroll->setToolTip(QStringLiteral("Reverse mousewheel scrolling in Flat View."));
    pageLayout->addWidget(m_invertMousewheelScroll);
    pageLayout->addStretch();
}

void SettingsDialog::saveHandlingSettings(const HandlingSettings& handling) {
    QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
    settings.setValue(QStringLiteral("handling/invert_mousewheel_scroll"), handling.invertMousewheelScroll);
}

QVector<KeyBinding> SettingsDialog::editedBindings() const {
    QVector<KeyBinding> result = m_bindings;
    for (int row = 0; row < result.size(); ++row) {
        result[row].sequence = m_shortcutEdits[row]->keySequence();
    }

    return result;
}

bool SettingsDialog::updateConflicts() {
    const QVector<KeyBinding> bindings = editedBindings();
    const QVector<int> conflicts = conflictingKeyBindings(bindings);
    for (int row = 0; row < m_shortcutEdits.size(); ++row) {
        setShortcutConflictStyle(m_shortcutEdits[row], conflicts.contains(row));
    }
    m_validationLabel->setText(conflicts.isEmpty() ? QString()
        : QStringLiteral("Conflicting keybinds: %1. Assign different sequences before saving.")
            .arg(bindings[conflicts.front()].label));

    return conflicts.isEmpty();
}

void SettingsDialog::resetShortcut(const int row) {
    m_shortcutEdits[row]->setKeySequence(m_bindings[row].defaultSequence);
}

void SettingsDialog::resetAllShortcuts() {
    for (int row = 0; row < m_bindings.size(); ++row) {
        resetShortcut(row);
    }
    updateConflicts();
}

void SettingsDialog::saveKeybinds(const QVector<KeyBinding>& bindings) {
    QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
    settings.beginGroup(QStringLiteral("keybinds"));
    for (const KeyBinding& binding : bindings) {
        if (binding.sequence == binding.defaultSequence) {
            settings.remove(binding.id);
        } else {
            settings.setValue(binding.id, binding.sequence.toString(QKeySequence::PortableText));
        }
    }
    settings.endGroup();
}

} // namespace infalsus::gui
