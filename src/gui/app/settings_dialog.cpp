#include "gui/app/settings_dialog.h"
#include "core/media_assets.h"
#include "gui/state/hitsound_bank.h"
#include "gui/state/playback_controller.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QEvent>
#include <QtCore/QSettings>
#include <QtGui/QKeyEvent>
#include <QtGui/QFontMetrics>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QColorDialog>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QKeySequenceEdit>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace infalsus::gui {

namespace {

constexpr int kNavigationWidth = 150;
constexpr int kSettingsWidth = 800;
constexpr int kSettingsHeight = 650;
constexpr int kControlSpacing = 8;
constexpr int kHitsoundLabelGapCharacters = 15;
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

class PlaytestKeyEdit final : public QKeySequenceEdit {
public:
    using QKeySequenceEdit::QKeySequenceEdit;

protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress
            || event->type() == QEvent::KeyRelease) {
            if (event->type() == QEvent::KeyPress) {
                const auto& key = *static_cast<QKeyEvent*>(event);
                if (!key.isAutoRepeat() && key.key() != Qt::Key_unknown) {
                    setKeySequence(QKeySequence(keyBindingCombination(key).key()));
                }
            }
            event->accept();
            return true;
        }
        return QKeySequenceEdit::event(event);
    }
};

} // namespace

SettingsDialog::SettingsDialog(KeyBindingRouter* keyBindings, const VisualSettings& visuals,
    const HandlingSettings& handling, const PlaytestSettings& playtest,
    HitsoundBank* hitsounds, PlaybackController* playback, QWidget* parent)
    : QDialog(parent)
    , m_keyBindings(keyBindings)
    , m_visuals(visuals)
    , m_playback(playback)
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
    navigation->addItems({QStringLiteral("Keybinds"), QStringLiteral("Visuals"), QStringLiteral("Handling"),
        QStringLiteral("Hitsounds"), QStringLiteral("Playtest")});
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
    buildHitsoundsPage(pages, hitsounds);
    buildPlaytestPage(pages, playtest);
    contentLayout->addWidget(navigation);
    contentLayout->addWidget(pages, 1);
    layout->addLayout(contentLayout, 1);
    layout->addWidget(buttons);
    connect(navigation, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(resetAll, &QPushButton::clicked, this, &SettingsDialog::resetAllShortcuts);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, m_playback, &PlaybackController::stopHitsoundPreview);
    updateConflicts();
    updatePlaytestConflicts();
}

VisualSettings SettingsDialog::selectedVisualSettings() const {
    VisualSettings result = m_visuals;
    result.customPlayheadColor = m_playheadColorMode->currentData().toBool();

    return result;
}

VisualSettings SettingsDialog::loadVisualSettings() {
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
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
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    HandlingSettings result;
    result.invertMousewheelScroll = settings.value(QStringLiteral("handling/invert_mousewheel_scroll"),
        result.invertMousewheelScroll).toBool();

    return result;
}

PlaytestSettings SettingsDialog::selectedPlaytestSettings() const {
    PlaytestSettings result;
    for (std::size_t lane = 0; lane < result.laneKeys.size(); ++lane) {
        const QKeySequence sequence = m_playtestLaneEdits[lane]->keySequence();
        result.laneKeys[lane] = sequence.isEmpty() ? Qt::Key_unknown : sequence[0].key();
    }
    const QKeySequence autoplay = m_playtestAutoplayEdit->keySequence();
    result.autoplayKey = autoplay.isEmpty() ? Qt::Key_unknown : autoplay[0].key();
    result.mouseSensitivity = m_mouseSensitivity->value();
    return result;
}

PlaytestSettings SettingsDialog::loadPlaytestSettings() {
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    return loadPlaytestSettings(settings);
}

PlaytestSettings SettingsDialog::loadPlaytestSettings(QSettings& settings) {
    PlaytestSettings result;
    for (std::size_t lane = 0; lane < result.laneKeys.size(); ++lane) {
        bool valid = false;
        const int key = settings.value(QStringLiteral("playtest/lane_%1").arg(lane), result.laneKeys[lane]).toInt(&valid);
        if (valid && key > 0 && key <= Qt::Key_unknown && key != Qt::Key_Escape) {
            result.laneKeys[lane] = static_cast<Qt::Key>(key);
        }
    }
    bool valid = false;
    const int autoplayKey = settings.value(QStringLiteral("playtest/autoplay_key"), result.autoplayKey).toInt(&valid);
    if (valid && autoplayKey > 0 && autoplayKey <= Qt::Key_unknown && autoplayKey != Qt::Key_Escape) {
        result.autoplayKey = static_cast<Qt::Key>(autoplayKey);
    }
    const double sensitivity = settings.value(QStringLiteral("playtest/mouse_sensitivity"),
        result.mouseSensitivity).toDouble(&valid);
    if (valid && std::isfinite(sensitivity)) {
        result.mouseSensitivity = std::clamp(sensitivity,
            kMinimumPlaytestMouseSensitivity, kMaximumPlaytestMouseSensitivity);
    }
    return result;
}

void SettingsDialog::restoreKeybinds(KeyBindingRouter* keyBindings) {
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
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
    const bool keybindsValid = updateConflicts();
    const bool playtestValid = updatePlaytestConflicts();
    if (!keybindsValid || !playtestValid) {
        return;
    }
    if (m_hitsounds->isDecoding()) {
        QMessageBox::information(this, QStringLiteral("Hitsounds"), QStringLiteral("Please wait for the sounds to finish loading."));
        return;
    }
    const QVector<KeyBinding> bindings = editedBindings();
    m_keyBindings->setBindings(bindings);
    saveKeybinds(bindings);
    saveVisualSettings(selectedVisualSettings());
    saveHandlingSettings(selectedHandlingSettings());
    savePlaytestSettings(selectedPlaytestSettings());
    m_hitsounds->save();
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
    rows->addRow(QStringLiteral("Playhead color"), controls);
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
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
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
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    settings.setValue(QStringLiteral("handling/invert_mousewheel_scroll"), handling.invertMousewheelScroll);
}

void SettingsDialog::buildPlaytestPage(QStackedWidget* pages, const PlaytestSettings& playtest) {
    auto* scrollArea = new QScrollArea(pages);
    auto* page = new QWidget(scrollArea);
    auto* pageLayout = new QVBoxLayout(page);
    auto* description = new QLabel(QStringLiteral("Choose one key for each playtest control. Clear a keybind to disable it."), page);
    auto* rows = new QFormLayout;
    auto* resetControls = new QHBoxLayout;
    auto* resetAll = new QPushButton(QStringLiteral("Reset All"), page);
    m_playtestValidationLabel = new QLabel(page);
    pages->addWidget(scrollArea);
    scrollArea->setWidgetResizable(true);
    scrollArea->setWidget(page);
    description->setWordWrap(true);
    pageLayout->addWidget(description);
    rows->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    for (std::size_t lane = 0; lane < m_playtestLaneEdits.size(); ++lane) {
        auto* controls = new QWidget(page);
        auto* controlsLayout = new QHBoxLayout(controls);
        const QKeySequence sequence = playtest.laneKeys[lane] == Qt::Key_unknown ? QKeySequence()
            : QKeySequence(playtest.laneKeys[lane]);
        auto* edit = new PlaytestKeyEdit(sequence, controls);
        auto* reset = new QPushButton(QStringLiteral("Reset"), controls);
        edit->setObjectName(QStringLiteral("playtest_lane_%1").arg(lane));
        edit->setClearButtonEnabled(true);
        edit->setMaximumSequenceLength(1);
        controlsLayout->setContentsMargins(0, 0, 0, 0);
        controlsLayout->setSpacing(kControlSpacing);
        controlsLayout->addWidget(edit, 1);
        controlsLayout->addWidget(reset);
        rows->addRow(QStringLiteral("Lane %1").arg(lane), controls);
        m_playtestLaneEdits[lane] = edit;
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [this] { updatePlaytestConflicts(); });
        connect(reset, &QPushButton::clicked, this, [this, lane] {
            m_playtestLaneEdits[lane]->setKeySequence(QKeySequence(kDefaultPlaytestLaneKeys[lane]));
        });
    }
    auto* autoplayControls = new QWidget(page);
    auto* autoplayLayout = new QHBoxLayout(autoplayControls);
    const QKeySequence autoplaySequence = playtest.autoplayKey == Qt::Key_unknown ? QKeySequence()
        : QKeySequence(playtest.autoplayKey);
    m_playtestAutoplayEdit = new PlaytestKeyEdit(autoplaySequence, autoplayControls);
    auto* autoplayReset = new QPushButton(QStringLiteral("Reset"), autoplayControls);
    m_playtestAutoplayEdit->setObjectName(QStringLiteral("playtest_autoplay_key"));
    m_playtestAutoplayEdit->setClearButtonEnabled(true);
    m_playtestAutoplayEdit->setMaximumSequenceLength(1);
    autoplayLayout->setContentsMargins(0, 0, 0, 0);
    autoplayLayout->setSpacing(kControlSpacing);
    autoplayLayout->addWidget(m_playtestAutoplayEdit, 1);
    autoplayLayout->addWidget(autoplayReset);
    rows->addRow(QStringLiteral("Toggle autoplay"), autoplayControls);
    connect(m_playtestAutoplayEdit, &QKeySequenceEdit::keySequenceChanged, this, [this] { updatePlaytestConflicts(); });
    connect(autoplayReset, &QPushButton::clicked, this, [this] {
        m_playtestAutoplayEdit->setKeySequence(QKeySequence(kDefaultPlaytestAutoplayKey));
    });
    auto* controls = new QWidget(page);
    auto* controlsLayout = new QHBoxLayout(controls);
    auto* reset = new QPushButton(QStringLiteral("Reset"), controls);
    m_mouseSensitivity = new QDoubleSpinBox(controls);
    m_mouseSensitivity->setObjectName(QStringLiteral("playtest_mouse_sensitivity"));
    m_mouseSensitivity->setRange(kMinimumPlaytestMouseSensitivity, kMaximumPlaytestMouseSensitivity);
    m_mouseSensitivity->setDecimals(2);
    m_mouseSensitivity->setSingleStep(0.1);
    m_mouseSensitivity->setSuffix(QStringLiteral("x"));
    m_mouseSensitivity->setValue(playtest.mouseSensitivity);
    m_mouseSensitivity->setToolTip(QStringLiteral("Multiply mouse movement during playtest."));
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(kControlSpacing);
    controlsLayout->addWidget(m_mouseSensitivity, 1);
    controlsLayout->addWidget(reset);
    rows->addRow(QStringLiteral("Mouse sensitivity"), controls);
    pageLayout->addLayout(rows);
    m_playtestValidationLabel->setWordWrap(true);
    m_playtestValidationLabel->setStyleSheet(QStringLiteral("color: #d07070;"));
    resetControls->addWidget(m_playtestValidationLabel, 1);
    resetControls->addWidget(resetAll);
    pageLayout->addLayout(resetControls);
    pageLayout->addStretch();
    connect(reset, &QPushButton::clicked, this, [this] {
        m_mouseSensitivity->setValue(kDefaultPlaytestMouseSensitivity);
    });
    connect(resetAll, &QPushButton::clicked, this, [this] {
        for (std::size_t lane = 0; lane < m_playtestLaneEdits.size(); ++lane) {
            m_playtestLaneEdits[lane]->setKeySequence(QKeySequence(kDefaultPlaytestLaneKeys[lane]));
        }
        m_playtestAutoplayEdit->setKeySequence(QKeySequence(kDefaultPlaytestAutoplayKey));
        m_mouseSensitivity->setValue(kDefaultPlaytestMouseSensitivity);
    });
}

bool SettingsDialog::updatePlaytestConflicts() {
    std::array<QKeySequenceEdit*, 7> edits{};
    std::copy(m_playtestLaneEdits.begin(), m_playtestLaneEdits.end(), edits.begin());
    edits.back() = m_playtestAutoplayEdit;
    std::array<bool, 7> conflicts{};
    QString message;
    for (std::size_t lane = 0; lane < edits.size(); ++lane) {
        const QKeySequence sequence = edits[lane]->keySequence();
        if (sequence.isEmpty()) {
            continue;
        }
        const Qt::Key key = sequence[0].key();
        if (key == Qt::Key_Escape || key == Qt::Key_unknown || sequence.count() != 1
            || sequence[0].keyboardModifiers() != Qt::NoModifier) {
            conflicts[lane] = true;
            message = key == Qt::Key_Escape ? QStringLiteral("Escape is reserved for returning to the editor.")
                : QStringLiteral("Choose a single key for each playtest control.");
        }
        for (std::size_t other = 0; other < lane; ++other) {
            if (!edits[other]->keySequence().isEmpty()
                && edits[other]->keySequence()[0].key() == key) {
                conflicts[lane] = conflicts[other] = true;
                if (message.isEmpty()) {
                    message = QStringLiteral("Conflicting playtest keybinds. Assign different keys before saving.");
                }
            }
        }
    }
    for (std::size_t lane = 0; lane < conflicts.size(); ++lane) {
        setShortcutConflictStyle(edits[lane], conflicts[lane]);
    }
    m_playtestValidationLabel->setText(message);
    return std::none_of(conflicts.begin(), conflicts.end(), [](const bool conflict) { return conflict; });
}

void SettingsDialog::savePlaytestSettings(const PlaytestSettings& playtest) {
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    for (std::size_t lane = 0; lane < playtest.laneKeys.size(); ++lane) {
        const QString id = QStringLiteral("playtest/lane_%1").arg(lane);
        if (playtest.laneKeys[lane] == kDefaultPlaytestLaneKeys[lane]) {
            settings.remove(id);
        } else {
            settings.setValue(id, static_cast<int>(playtest.laneKeys[lane]));
        }
    }
    if (playtest.autoplayKey == kDefaultPlaytestAutoplayKey) {
        settings.remove(QStringLiteral("playtest/autoplay_key"));
    } else {
        settings.setValue(QStringLiteral("playtest/autoplay_key"), static_cast<int>(playtest.autoplayKey));
    }
    if (playtest.mouseSensitivity == kDefaultPlaytestMouseSensitivity) {
        settings.remove(QStringLiteral("playtest/mouse_sensitivity"));
    } else {
        settings.setValue(QStringLiteral("playtest/mouse_sensitivity"), playtest.mouseSensitivity);
    }
}

void SettingsDialog::buildHitsoundsPage(QStackedWidget* pages, HitsoundBank* hitsounds) {
    auto* scrollArea = new QScrollArea(pages);
    auto* page = new QWidget(scrollArea);
    auto* pageLayout = new QVBoxLayout(page);
    auto* rows = new QFormLayout;
    m_hitsounds = new HitsoundBank(m_playback->audioFormat(), this);
    m_pendingReplacements.resize(kHitsoundDescriptions.size());

    pages->addWidget(scrollArea);
    scrollArea->setWidgetResizable(true);
    scrollArea->setWidget(page);
    rows->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    rows->setHorizontalSpacing(QFontMetrics(page->font()).averageCharWidth() * kHitsoundLabelGapCharacters);
    for (int index = 0; index < static_cast<int>(kHitsoundDescriptions.size()); ++index) {
        auto* controls = new QWidget(page);
        auto* layout = new QHBoxLayout(controls);
        auto* sound = new QPushButton(hitsounds->files()[index].name, controls);
        auto* pick = new QPushButton(QStringLiteral("Pick file"), controls);
        sound->setEnabled(false);
        sound->setToolTip(QStringLiteral("Play sound"));
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(kControlSpacing);
        layout->addWidget(sound, 1);
        layout->addWidget(pick);
        rows->addRow(QString::fromLatin1(kHitsoundDescriptions[index].label), controls);
        m_soundButtons.append(sound);
        m_pickButtons.append(pick);
        connect(sound, &QPushButton::clicked, this, [this, index] {
            m_playback->previewHitsound(m_hitsounds->samples(index));
        });
        connect(pick, &QPushButton::clicked, this, [this, index] { pickHitsound(index); });
    }
    pageLayout->addLayout(rows);
    pageLayout->addStretch();
    connect(m_hitsounds, &HitsoundBank::soundReady, this, [this](const int index) {
        m_soundButtons[index]->setText(m_hitsounds->files()[index].name);
        m_soundButtons[index]->setEnabled(true);
        m_soundButtons[index]->setToolTip(QStringLiteral("Play sound"));
        m_pickButtons[index]->setEnabled(true);
        m_pendingReplacements[index] = false;
    });
    connect(m_hitsounds, &HitsoundBank::soundError, this, [this](const int index, const QString& message) {
        m_pickButtons[index]->setEnabled(true);
        m_soundButtons[index]->setEnabled(!m_hitsounds->samples(index).isEmpty());
        m_soundButtons[index]->setToolTip(message);
        if (m_pendingReplacements[index]) {
            m_pendingReplacements[index] = false;
            QMessageBox::warning(this, QStringLiteral("Hitsound Error"), message);
        }
    });
    m_hitsounds->load(hitsounds->files());
}

void SettingsDialog::pickHitsound(const int index) {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Pick Hitsound"), QString(), audioFileDialogFilter());
    if (path.isEmpty()) {
        return;
    }
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Hitsound Error"), source.errorString());
        return;
    }
    const QByteArray data = source.readAll();
    if (data.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Hitsound Error"), QStringLiteral("The sound file is empty or could not be read."));
        return;
    }
    m_pendingReplacements[index] = true;
    m_pickButtons[index]->setEnabled(false);
    m_soundButtons[index]->setEnabled(false);
    m_hitsounds->replace(index, {QFileInfo(path).fileName(), data, true});
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
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
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
