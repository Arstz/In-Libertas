#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVector>
#include <QtGui/QKeySequence>

class QAction;
class QKeyEvent;
class QTimer;
class QWidget;

namespace infalsus::gui {

enum class KeyCommand {
    NewProject, OpenProject, ImportSpc, SaveProject, SaveProjectAs, ExportProject,
    Undo, Redo, Cut, Copy, Paste, DeleteSelection, SelectAll, MirrorSelection,
    FlipSelectionVertically, ResnapAll, RefreshVerification,
    PlaceTool, SelectTool, MoveTool, ZoomIn, ZoomInAlternate, ZoomOut, ZoomOutAlternate,
    TogglePlayback, IncreasePlaybackRate, DecreasePlaybackRate,
    SeekBackward, SeekForward, IncreaseDivisor, DecreaseDivisor, CycleFlatMode,
    ResetLayout, Settings,
};

struct KeyBinding {
    KeyCommand command;
    QString id;
    QString label;
    QKeySequence defaultSequence;
    QKeySequence sequence;
    bool autoRepeat = false;
};

[[nodiscard]] QKeyCombination keyBindingCombination(const QKeyEvent& event);
[[nodiscard]] QVector<int> conflictingKeyBindings(const QVector<KeyBinding>& bindings);

class KeyBindingRouter final : public QObject {
public:
    explicit KeyBindingRouter(QWidget* window);

    void registerAction(KeyCommand command, QAction* action, bool showInMenu = true);
    [[nodiscard]] QVector<KeyBinding> bindings() const;
    void setBindings(const QVector<KeyBinding>& bindings);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct ActionBinding {
        KeyBinding binding;
        QPointer<QAction> action;
        QString text;
        bool showInMenu = true;
    };

    [[nodiscard]] bool canRouteKeys() const;
    [[nodiscard]] bool routeKeyPress(QKeyEvent& event);
    [[nodiscard]] bool matchesPendingSequence(const QKeySequence& sequence) const;
    void updateActionText(ActionBinding& binding);
    void clearPendingSequence();

    QPointer<QWidget> m_window;
    QVector<ActionBinding> m_actions;
    QVector<QKeyCombination> m_pendingSequence;
    QTimer* m_sequenceTimer = nullptr;
};

} // namespace infalsus::gui
