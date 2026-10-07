#include "gui/key_bindings.h"

#include <QtCore/QEvent>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QAbstractSpinBox>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QKeySequenceEdit>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <iterator>

namespace infalsus::gui {

namespace {

constexpr int kSequenceTimeoutMilliseconds = 1000;
constexpr int kMaximumSequenceLength = 4;

struct CommandDefinition {
    KeyCommand command;
    const char* id;
    const char* label;
    const char* sequence;
    bool autoRepeat = false;
};

constexpr CommandDefinition kCommands[]{
    {KeyCommand::NewProject, "new_project", "File / New project", "Ctrl+N"},
    {KeyCommand::OpenProject, "open_project", "File / Open project", "Ctrl+O"},
    {KeyCommand::ImportSpc, "import_spc", "File / Import SPC", ""},
    {KeyCommand::SaveProject, "save_project", "File / Save project", "Ctrl+S"},
    {KeyCommand::SaveProjectAs, "save_project_as", "File / Save project as", "Ctrl+Shift+S"},
    {KeyCommand::ExportProject, "export_project", "File / Export project", ""},
    {KeyCommand::Undo, "undo", "Edit / Undo", "Ctrl+Z", true},
    {KeyCommand::Redo, "redo", "Edit / Redo", "Ctrl+Y", true},
    {KeyCommand::Cut, "cut", "Edit / Cut", "Ctrl+X"},
    {KeyCommand::Copy, "copy", "Edit / Copy", "Ctrl+C"},
    {KeyCommand::Paste, "paste", "Edit / Paste", "Ctrl+V"},
    {KeyCommand::DeleteSelection, "delete_selection", "Edit / Delete selection", "Del", true},
    {KeyCommand::SelectAll, "select_all", "Edit / Select all visible", "Ctrl+A"},
    {KeyCommand::MirrorSelection, "mirror_selection", "Edit / Mirror selection", "Ctrl+H"},
    {KeyCommand::FlipSelectionVertically, "flip_selection_vertically", "Edit / Flip selection vertically", "Ctrl+J"},
    {KeyCommand::ToggleZoneGrouping, "toggle_zone_grouping", "Edit / Grouping", "Ctrl+G"},
    {KeyCommand::ResnapAll, "resnap_all", "Edit / Resnap all hit objects", "Ctrl+Shift+E"},
    {KeyCommand::RefreshVerification, "refresh_verification", "Edit / Refresh verification", "Ctrl+Shift+A"},
    {KeyCommand::PlaceTool, "place_tool", "Tools / Place", "1"},
    {KeyCommand::SelectTool, "select_tool", "Tools / Select", "F"},
    {KeyCommand::MoveTool, "move_tool", "Tools / Move", "V"},
    {KeyCommand::ZoomIn, "zoom_in", "View / Zoom in", "+", true},
    {KeyCommand::ZoomInAlternate, "zoom_in_alternate", "View / Zoom in (alternate)", "=", true},
    {KeyCommand::ZoomOut, "zoom_out", "View / Zoom out", "-", true},
    {KeyCommand::ZoomOutAlternate, "zoom_out_alternate", "View / Zoom out (alternate)", "_", true},
    {KeyCommand::TogglePlayback, "toggle_playback", "Playback / Play or pause", "Space"},
    {KeyCommand::IncreasePlaybackRate, "increase_playback_rate", "Playback / Increase speed", "[", true},
    {KeyCommand::DecreasePlaybackRate, "decrease_playback_rate", "Playback / Decrease speed", "]", true},
    {KeyCommand::SeekBackward, "seek_backward", "Playback / Seek backward", "Left", true},
    {KeyCommand::SeekForward, "seek_forward", "Playback / Seek forward", "Right", true},
    {KeyCommand::IncreaseDivisor, "increase_divisor", "View / Increase divisor", "Up", true},
    {KeyCommand::DecreaseDivisor, "decrease_divisor", "View / Decrease divisor", "Down", true},
    {KeyCommand::CycleFlatMode, "cycle_flat_mode", "View / Cycle Ground, Sky, Both", "Tab"},
    {KeyCommand::ResetLayout, "reset_layout", "Window / Reset layout", ""},
    {KeyCommand::Settings, "settings", "Settings / Open settings", "Ctrl+K"},
};

struct PhysicalKey {
    quint32 scanCode;
    Qt::Key key;
};

[[nodiscard]] Qt::Key layoutIndependentKey(const QKeyEvent& event) {
#ifdef Q_OS_WIN
    constexpr PhysicalKey kPhysicalKeys[]{
        {0x02, Qt::Key_1}, {0x03, Qt::Key_2}, {0x04, Qt::Key_3},
        {0x05, Qt::Key_4}, {0x06, Qt::Key_5}, {0x07, Qt::Key_6},
        {0x08, Qt::Key_7}, {0x09, Qt::Key_8}, {0x0a, Qt::Key_9}, {0x0b, Qt::Key_0},
        {0x10, Qt::Key_Q}, {0x11, Qt::Key_W}, {0x12, Qt::Key_E},
        {0x13, Qt::Key_R}, {0x14, Qt::Key_T}, {0x15, Qt::Key_Y},
        {0x16, Qt::Key_U}, {0x17, Qt::Key_I}, {0x18, Qt::Key_O}, {0x19, Qt::Key_P},
        {0x1e, Qt::Key_A}, {0x1f, Qt::Key_S}, {0x20, Qt::Key_D},
        {0x21, Qt::Key_F}, {0x22, Qt::Key_G}, {0x23, Qt::Key_H},
        {0x24, Qt::Key_J}, {0x25, Qt::Key_K}, {0x26, Qt::Key_L},
        {0x2c, Qt::Key_Z}, {0x2d, Qt::Key_X}, {0x2e, Qt::Key_C},
        {0x2f, Qt::Key_V}, {0x30, Qt::Key_B}, {0x31, Qt::Key_N}, {0x32, Qt::Key_M},
    };
    const quint32 scanCode = event.nativeScanCode() & 0xff;
    const auto found = std::find_if(std::begin(kPhysicalKeys), std::end(kPhysicalKeys),
        [scanCode](const PhysicalKey& entry) { return entry.scanCode == scanCode; });
    if (found != std::end(kPhysicalKeys)) {
        return found->key;
    }
#endif

    return static_cast<Qt::Key>(event.key());
}

[[nodiscard]] bool isTextEntryWidget(const QWidget* widget) {
    for (const QWidget* current = widget; current != nullptr; current = current->parentWidget()) {
        const auto* combo = qobject_cast<const QComboBox*>(current);
        if (qobject_cast<const QLineEdit*>(current) != nullptr
            || qobject_cast<const QAbstractSpinBox*>(current) != nullptr
            || qobject_cast<const QTextEdit*>(current) != nullptr
            || qobject_cast<const QPlainTextEdit*>(current) != nullptr
            || qobject_cast<const QKeySequenceEdit*>(current) != nullptr
            || (combo != nullptr && combo->isEditable())) {
            return true;
        }
    }

    return false;
}

[[nodiscard]] bool isModifierKey(const QKeyEvent& event) {
    return event.key() == Qt::Key_Shift || event.key() == Qt::Key_Control
        || event.key() == Qt::Key_Alt || event.key() == Qt::Key_Meta || event.key() == Qt::Key_AltGr;
}

} // namespace

QKeyCombination keyBindingCombination(const QKeyEvent& event) {
    Qt::KeyboardModifiers modifiers = event.modifiers()
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    Qt::Key key = layoutIndependentKey(event);
    if (key == Qt::Key_Backtab) {
        key = Qt::Key_Tab;
        modifiers |= Qt::ShiftModifier;
    }
    if (key == Qt::Key_Plus || key == Qt::Key_Underscore) {
        modifiers &= ~Qt::ShiftModifier;
    }

    return QKeyCombination(modifiers, key);
}

QVector<int> conflictingKeyBindings(const QVector<KeyBinding>& bindings) {
    QVector<int> conflicts;
    for (int left = 0; left < bindings.size(); ++left) {
        if (bindings[left].sequence.isEmpty()) {
            continue;
        }
        for (int right = left + 1; right < bindings.size(); ++right) {
            if (!bindings[right].sequence.isEmpty()
                && (bindings[left].sequence.matches(bindings[right].sequence) != QKeySequence::NoMatch
                    || bindings[right].sequence.matches(bindings[left].sequence) != QKeySequence::NoMatch)) {
                if (!conflicts.contains(left)) {
                    conflicts.append(left);
                }
                if (!conflicts.contains(right)) {
                    conflicts.append(right);
                }
            }
        }
    }

    return conflicts;
}

KeyBindingRouter::KeyBindingRouter(QWidget* window)
    : QObject(window)
    , m_window(window)
    , m_sequenceTimer(new QTimer(this)) {
    m_sequenceTimer->setSingleShot(true);
    m_sequenceTimer->setInterval(kSequenceTimeoutMilliseconds);
    connect(m_sequenceTimer, &QTimer::timeout, this, &KeyBindingRouter::clearPendingSequence);
    qApp->installEventFilter(this);
}

void KeyBindingRouter::registerAction(const KeyCommand command, QAction* action, const bool showInMenu) {
    const auto definition = std::find_if(std::begin(kCommands), std::end(kCommands),
        [command](const CommandDefinition& item) { return item.command == command; });
    Q_ASSERT(definition != std::end(kCommands));
    Q_ASSERT(action != nullptr);
    const QKeySequence sequence(QString::fromLatin1(definition->sequence), QKeySequence::PortableText);
    ActionBinding binding{
        {command, QString::fromLatin1(definition->id), QString::fromUtf8(definition->label),
            sequence, sequence, definition->autoRepeat},
        action, action->text(), showInMenu};
    action->setShortcut({});
    action->setAutoRepeat(definition->autoRepeat);
    updateActionText(binding);
    m_actions.append(std::move(binding));
}

QVector<KeyBinding> KeyBindingRouter::bindings() const {
    QVector<KeyBinding> result;
    for (const ActionBinding& action : m_actions) {
        result.append(action.binding);
    }
    std::sort(result.begin(), result.end(), [](const KeyBinding& left, const KeyBinding& right) {
        return left.command < right.command;
    });

    return result;
}

void KeyBindingRouter::setBindings(const QVector<KeyBinding>& bindings) {
    if (!conflictingKeyBindings(bindings).isEmpty()) {
        return;
    }
    for (ActionBinding& action : m_actions) {
        const auto found = std::find_if(bindings.cbegin(), bindings.cend(),
            [&action](const KeyBinding& item) { return item.command == action.binding.command; });
        if (found != bindings.cend()) {
            action.binding.sequence = found->sequence;
            updateActionText(action);
        }
    }
    clearPendingSequence();
}

bool KeyBindingRouter::eventFilter(QObject* watched, QEvent* event) {
    Q_UNUSED(watched);
    if (event->type() == QEvent::ApplicationDeactivate || event->type() == QEvent::WindowDeactivate
        || event->type() == QEvent::FocusOut) {
        clearPendingSequence();
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride) {
        return false;
    }
    auto& keyEvent = *static_cast<QKeyEvent*>(event);
    if (!canRouteKeys()) {
        clearPendingSequence();
        return false;
    }
    if (isModifierKey(keyEvent)) {
        return false;
    }
    if (event->type() == QEvent::ShortcutOverride) {
        const QKeySequence pressed(keyBindingCombination(keyEvent));
        const bool matched = std::any_of(m_actions.cbegin(), m_actions.cend(),
            [&pressed](const ActionBinding& action) {
                return action.action != nullptr && action.action->isEnabled()
                    && !action.binding.sequence.isEmpty()
                    && pressed.matches(QKeySequence(action.binding.sequence[0])) == QKeySequence::ExactMatch;
            });
        if (matched || !m_pendingSequence.isEmpty()) {
            event->accept();
            return true;
        }

        return false;
    }

    return routeKeyPress(keyEvent);
}

bool KeyBindingRouter::canRouteKeys() const {
    const QWidget* focus = QApplication::focusWidget();

    return m_window != nullptr && QApplication::activeWindow() == m_window
        && QApplication::activeModalWidget() == nullptr && QApplication::activePopupWidget() == nullptr
        && !isTextEntryWidget(focus);
}

bool KeyBindingRouter::routeKeyPress(QKeyEvent& event) {
    const QKeyCombination combination = keyBindingCombination(event);
    bool partialMatch = false;
    if (combination.key() == Qt::Key_unknown) {
        return false;
    }
    m_pendingSequence.append(combination);
    if (m_pendingSequence.size() > kMaximumSequenceLength) {
        clearPendingSequence();
        m_pendingSequence.append(combination);
    }
    for (const ActionBinding& action : std::as_const(m_actions)) {
        if (action.action == nullptr || !action.action->isEnabled() || action.binding.sequence.isEmpty()
            || !matchesPendingSequence(action.binding.sequence)) {
            continue;
        }
        if (action.binding.sequence.count() == m_pendingSequence.size()) {
            QPointer<QAction> triggeredAction = action.action;
            const bool trigger = !event.isAutoRepeat() || action.binding.autoRepeat;
            clearPendingSequence();
            if (trigger) {
                triggeredAction->trigger();
            }
            event.accept();
            return true;
        }
        partialMatch = true;
    }
    if (partialMatch) {
        m_sequenceTimer->start();
        event.accept();
        return true;
    }
    const bool retry = m_pendingSequence.size() > 1;
    clearPendingSequence();

    return retry && routeKeyPress(event);
}

bool KeyBindingRouter::matchesPendingSequence(const QKeySequence& sequence) const {
    if (sequence.count() < m_pendingSequence.size()) {
        return false;
    }
    for (int index = 0; index < m_pendingSequence.size(); ++index) {
        if (sequence[index] != m_pendingSequence[index]) {
            return false;
        }
    }

    return true;
}

void KeyBindingRouter::updateActionText(ActionBinding& binding) {
    const QString sequence = binding.binding.sequence.toString(QKeySequence::NativeText);
    if (binding.showInMenu) {
        binding.action->setText(sequence.isEmpty() ? binding.text
            : QStringLiteral("%1\t%2").arg(binding.text, sequence));
    }
    binding.action->setToolTip(sequence.isEmpty() ? binding.text
        : QStringLiteral("%1 (%2)").arg(binding.text, sequence));
}

void KeyBindingRouter::clearPendingSequence() {
    m_pendingSequence.clear();
    m_sequenceTimer->stop();
}

} // namespace infalsus::gui
