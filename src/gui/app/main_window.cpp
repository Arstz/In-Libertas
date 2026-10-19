#include "gui/app/main_window.h"
#include "gui/app/settings_dialog.h"
#include "gui/app/chartbox_dialog.h"
#include "gui/key_bindings.h"

#include "core/chart_project.h"
#include "core/audio_exporter.h"
#include "core/media_assets.h"
#include "core/timing_analyzer.h"
#include "gui/canvas/flat_view.h"
#include "gui/state/editor_state.h"
#include "gui/widgets/metadata_widget.h"
#include "gui/widgets/properties_panel.h"
#include "gui/widgets/timeline_widget.h"
#include "gui/widgets/playback_controls.h"
#include "gui/widgets/timing_widget.h"
#include "gui/widgets/verification_widget.h"
#include "core/chart_document.h"
#include "core/project_document.h"
#include "core/project_assets.h"
#include "core/project_format.h"
#include "core/project_export.h"
#include "gui/canvas/conveyor_view.h"
#include "gui/state/playback_controller.h"

#include <QtCore/QDir>
#include <QtCore/QPromise>
#include <QtCore/QThreadPool>
#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtMultimedia/QAudioFormat>
#include <QtGui/QActionGroup>
#include <QtGui/QFontMetrics>
#include <QtGui/QKeyEvent>
#include <QtGui/QImage>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QProgressDialog>
#include <QtGui/QShowEvent>
#include <QtGui/QCloseEvent>
#include <QtGui/QCursor>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QSlider>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace infalsus::gui {

namespace {

constexpr int kRecentProjectLimit = 10;
constexpr int kToolsToolbarMinimumWidth = 92;
constexpr int kToolButtonTextPadding = 8;

[[nodiscard]] bool isProjectDocumentFile(const QFileInfo& fileInfo) {
    return fileInfo.isFile() && fileInfo.suffix().compare(QStringLiteral("10no"), Qt::CaseInsensitive) == 0;
}

[[nodiscard]] QString formatPreciseTimestamp(const qint64 positionMilliseconds) {
    constexpr qint64 kMillisecondsPerSecond = 1000;
    const qint64 clampedPosition = std::max<qint64>(0, positionMilliseconds);
    const qint64 totalSeconds = clampedPosition / kMillisecondsPerSecond;
    const qint64 milliseconds = clampedPosition % kMillisecondsPerSecond;

    return QStringLiteral("%1:%2:%3")
        .arg(totalSeconds / 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'))
        .arg(milliseconds, 3, 10, QLatin1Char('0'));
}

[[nodiscard]] bool nativeAltHeld() {
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

[[nodiscard]] double wheelSteps(const QWheelEvent& event) {
    const double angleSteps = static_cast<double>(event.angleDelta().y()) / 120.0;
    if (std::abs(angleSteps) >= 0.001) {
        return angleSteps;
    }

    return static_cast<double>(event.pixelDelta().y()) / 120.0;
}

[[nodiscard]] qreal logarithmicVolumeForPercent(const int percent) {
    const qreal normalized = std::clamp(static_cast<qreal>(percent) / 100.0, qreal(0.0), qreal(1.0));
    if (normalized <= 0.0) {
        return 0.0;
    }

    // Present the editor's control as 0–100%, but map each equal slider
    // increment to an equal dB interval.  -60 dB is effectively silent while
    // preserving enough precision to make low-volume adjustments useful.
    constexpr qreal kMinimumDecibels = -60.0;
    const qreal decibels = kMinimumDecibels * (1.0 - normalized);
    return std::pow(qreal(10.0), decibels / 20.0);
}

[[nodiscard]] bool isSkyHitObject(const ChartNote& hitObject) {
    return hitObject.kind == NoteKind::Sky || hitObject.kind == NoteKind::Flick;
}

[[nodiscard]] QUrl inMemoryAudioHint(const QString& fileName) {
    const QString safeFileName = QFileInfo(fileName).fileName();
    return QUrl(QStringLiteral("memory://project/%1").arg(
        QString::fromLatin1(QUrl::toPercentEncoding(safeFileName))));
}

struct ImportedSpcIdentity {
    QString songName;
    Difficulty difficulty = Difficulty::Minimal;
};

[[nodiscard]] ImportedSpcIdentity identityForImportedSpc(const QFileInfo& chartFileInfo) {
    const QString stem = chartFileInfo.completeBaseName();
    static const QRegularExpression kDifficultyIndexSuffix(QStringLiteral("^(.+)([0-3])$"));
    const QRegularExpressionMatch match = kDifficultyIndexSuffix.match(stem);
    if (!match.hasMatch()) {
        return {.songName = stem};
    }

    return {
        .songName = match.captured(1),
        .difficulty = difficultyForIndex(match.captured(2).toInt()),
    };
}

[[nodiscard]] QString jacketPathForImportedSpc(const QFileInfo& chartFileInfo, const QString& songName) {
    const QDir folder(chartFileInfo.absolutePath());
    QStringList candidates;
    const QStringList stems{songName, chartFileInfo.completeBaseName(),
        QStringLiteral("jacketLarge"), QStringLiteral("jacket"), QStringLiteral("jacketSmall")};
    const QStringList filters = imageNameFilters();
    for (const QString& stem : stems) {
        candidates.append(stem + QStringLiteral(".png"));
        for (const QString& filter : filters) {
            candidates.append(stem + filter.mid(1));
        }
    }
    candidates.removeDuplicates();
    for (const QString& fileName : candidates) {
        const QString path = folder.filePath(fileName);
        if (!QFileInfo::exists(path)) {
            continue;
        }
        QString error;
        if (readJacketImage(path, nullptr, &error)) {
            return path;
        }
    }

    return {};
}


[[nodiscard]] QStringList exportWarningsFor(const ChartProject& project) {
    QStringList warnings;
    if (project.metadata.songName.trimmed().isEmpty()) {
        warnings.append(QStringLiteral("Song name is empty."));
    }
    if (project.metadata.artistName.trimmed().isEmpty()) {
        warnings.append(QStringLiteral("Artist name is empty."));
    }
    if (project.metadata.previewStartSeconds == 0.0 || project.metadata.previewEndSeconds == 0.0) {
        warnings.append(QStringLiteral("Preview time is 0 ms."));
    }
    if (project.metadata.jacketDesigner.trimmed().isEmpty()) {
        warnings.append(QStringLiteral("Jacket designer name is empty."));
    }
    for (int index = 0; index < 4; ++index) {
        const DifficultyChart& chart = project.difficulties.at(index);
        if (chart.hitObjects.notes.isEmpty()) {
            continue;
        }
        if (chart.metadata.charters.trimmed().isEmpty()) {
            warnings.append(QStringLiteral("%1 charters are empty.").arg(difficultyName(difficultyForIndex(index))));
        }
        if (chart.metadata.rating > 19) {
            warnings.append(QStringLiteral("%1 rating is higher than 19 and will affect the UI.")
                .arg(difficultyName(difficultyForIndex(index))));
        }
    }
    return warnings;
}

[[nodiscard]] ChartProject blankProject(const QString& songName) {
    ChartProject project;
    project.metadata.songName = songName;
    for (int index = 0; index < 4; ++index) {
        const Difficulty difficulty = difficultyForIndex(index);
        project.difficulties.at(index) = {
            .difficulty = difficulty,
            .metadata = {},
            .hitObjects = {.name = songName},
            .timingPoints = {TimingPoint{}},
            .speedEvents = {SpeedEvent{}},
        };
        if (difficulty == Difficulty::Minimal) {
            project.difficulties.at(index).laneEvents = {{.timeMilliseconds = 0, .lane = 0, .enabled = false},
                {.timeMilliseconds = 0, .lane = 4, .enabled = false}};
        } else if (difficulty == Difficulty::Evolved) {
            project.difficulties.at(index).laneEvents = {{.timeMilliseconds = 0, .lane = 4, .enabled = false}};
        }
    }

    return project;
}

void analyzeDecodedTiming(QPromise<TimingEstimate>& promise, const QByteArray& samples,
    const QAudioFormat& format) {
    constexpr qint64 kMinimumAnalysisDurationSeconds = 60;
    constexpr qint64 kMaximumAnalysisDurationSeconds = 180;
    const int sampleRate = format.sampleRate();
    const int channels = format.channelCount();
    const qsizetype frameCount = std::min<qint64>(samples.size() / format.bytesPerFrame(),
        static_cast<qint64>(sampleRate) * kMaximumAnalysisDurationSeconds);
    const float* audio = reinterpret_cast<const float*>(samples.constData());
    InitialTimingAnalyzer analyzer(sampleRate);
    QVector<float> monoSamples(sampleRate);

    for (qsizetype firstFrame = 0; firstFrame < frameCount; firstFrame += sampleRate) {
        if (promise.isCanceled()) {
            return;
        }
        const qsizetype count = std::min<qsizetype>(sampleRate, frameCount - firstFrame);
        for (qsizetype frame = 0; frame < count; ++frame) {
            float sample = -1.0F;
            for (int channel = 0; channel < channels; ++channel) {
                sample = std::max(sample, audio[(firstFrame + frame) * channels + channel]);
            }
            monoSamples[frame] = std::isfinite(sample) ? sample : 0.0F;
        }
        analyzer.appendSamples(monoSamples.constData(), count);
        if (analyzer.sampleCount() >= static_cast<qint64>(sampleRate) * kMinimumAnalysisDurationSeconds) {
            const TimingEstimate estimate = analyzer.estimate();
            if (estimate.supportingIntervals >= 24 && estimate.confidence() >= 0.15) {
                promise.addResult(estimate);
                return;
            }
        }
    }
    promise.addResult(analyzer.estimate());
}

[[nodiscard]] QWidget* createPanel(
    const QString& title,
    QWidget* content,
    QWidget* parent,
    QWidget* titleAccessory = nullptr) {
    auto* panel = new QFrame(parent);
    panel->setWindowTitle(title);
    auto* layout = new QVBoxLayout(panel);
    auto* titleHost = new QWidget(panel);
    auto* titleLayout = new QHBoxLayout(titleHost);
    auto* titleLabel = new QLabel(title, titleHost);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(3);
    titleLabel->setFrameStyle(QFrame::Panel | QFrame::Raised);
    titleLabel->setMargin(3);
    titleLayout->addWidget(titleLabel, 1);
    if (titleAccessory != nullptr) {
        titleLayout->addWidget(titleAccessory);
    }
    layout->addWidget(titleHost);
    layout->addWidget(content, 1);
    panel->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);

    return panel;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    m_state = new EditorState(this);
    m_keyBindings = new KeyBindingRouter(this);
    m_playback = new PlaybackController(this);
    m_hitsounds = new HitsoundBank(m_playback->audioFormat(), this);
    m_hitsounds->load(HitsoundBank::loadFiles());
    m_playback->setHitsoundMix(m_hitsounds->mix());
    buildInterface();
    buildWorkspace();
    buildToolBar();
    registerNavigationKeybinds();
    SettingsDialog::restoreKeybinds(m_keyBindings);
    m_visualSettings = SettingsDialog::loadVisualSettings();
    m_flatView->setPlayheadColor(m_visualSettings.effectivePlayheadColor());
    m_handlingSettings = SettingsDialog::loadHandlingSettings();
    m_flatView->setInvertMousewheelScroll(m_handlingSettings.invertMousewheelScroll);
    m_playtestSettings = SettingsDialog::loadPlaytestSettings();
    m_viewer->setPlaytestLaneKeys(m_playtestSettings.laneKeys);
    restoreLayout();
    qApp->installEventFilter(this);
    m_playtestTimer = new QTimer(this);
    m_playtestTimer->setTimerType(Qt::PreciseTimer);
    m_playtestTimer->setInterval(4);
    connect(m_playtestTimer, &QTimer::timeout, this, &MainWindow::updatePlaytest);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (m_playtest) {
        if (event->type() == QEvent::ApplicationDeactivate
            || (watched == this && event->type() == QEvent::WindowDeactivate)) {
            leavePlaytest();
        } else if (watched == this && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
            m_viewer->setGeometry(rect());
            constrainPlaytestCursor();
        } else if (event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        } else if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (key->isAutoRepeat()) {
                return true;
            }
            const bool down = event->type() == QEvent::KeyPress;
            if (down && key->key() == Qt::Key_Escape) {
                leavePlaytest();
                return true;
            }
            if (!m_playtestFinished) {
                const int code = keyBindingCombination(*key).key();
                const auto& keys = m_playtestSettings.laneKeys;
                const auto found = std::find(keys.begin(), keys.end(), code);
                if (code != Qt::Key_unknown && found != keys.end()) {
                    const int lane = static_cast<int>(std::distance(keys.begin(), found));
                    const auto clock = samplePlaytestClock();
                    const double now = clock.monotonicTime;
                    const double eventTime = now + static_cast<std::int32_t>(static_cast<DWORD>(key->timestamp()) - GetTickCount()) / 1000.0;
                    const double timestamp = key->timestamp() == 0 ? now : std::min(now, eventTime);
                    if (down) {
                        m_playtestSession.press(lane, clock.chartTime, now, timestamp);
                    } else {
                        m_playtestSession.release(lane, clock.chartTime, now, timestamp);
                    }
                    publishPlaytest();
                }
            }
            return true;
        } else if (event->type() == QEvent::Wheel || event->type() == QEvent::ContextMenu
            || event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseButtonDblClick) {
            return true;
        }
    }
    if (QApplication::activeModalWidget() != nullptr && QApplication::activeModalWidget() != this) {
        return QMainWindow::eventFilter(watched, event);
    }
    if ((watched == m_flatView || watched == m_toolsToolbar) && event->type() == QEvent::Resize) {
        QTimer::singleShot(0, this, &MainWindow::updateVolumeSliderPlacement);
    }

    if (event->type() == QEvent::ApplicationDeactivate || event->type() == QEvent::WindowDeactivate) {
        m_altHeld = false;
    }

    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Alt) {
            m_altHeld = event->type() == QEvent::KeyPress;
        }
    }

    if (event->type() == QEvent::Wheel) {
        const auto* wheelEvent = static_cast<QWheelEvent*>(event);
        const QPoint flatPosition = m_flatView->mapFromGlobal(wheelEvent->globalPosition().toPoint());
        const bool isOverFlatView = m_flatView->rect().contains(flatPosition);
        const bool isAltHeld = m_altHeld || nativeAltHeld() || (wheelEvent->modifiers() & Qt::AltModifier) != 0;
        if (isOverFlatView && isAltHeld) {
            adjustFlatZoom(wheelSteps(*wheelEvent));
            return true;
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType == "windows_generic_MSG" && message != nullptr) {
        const auto* nativeMessage = static_cast<MSG*>(message);
        if (m_playtest && nativeMessage->message == WM_INPUT && !m_playtestFinished) {
            RAWINPUT input{};
            UINT size = sizeof(input);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(nativeMessage->lParam), RID_INPUT,
                    &input, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)
                && input.header.dwType == RIM_TYPEMOUSE
                && (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 && input.data.mouse.lLastX != 0) {
                const auto clock = samplePlaytestClock();
                const double now = clock.monotonicTime;
                const double eventTime = now + static_cast<std::int32_t>(nativeMessage->time - GetTickCount()) / 1000.0;
                m_playtestSession.move(input.data.mouse.lLastX * m_playtestSettings.mouseSensitivity
                        / m_viewer->gameplayMouseWidth(),
                    clock.chartTime, now, std::min(now, eventTime));
            }
        }
        if (m_playtest) {
            return QMainWindow::nativeEvent(eventType, message, result);
        }
        if (nativeMessage->message == WM_MOUSEWHEEL && nativeAltHeld()) {
            const auto lParam = static_cast<DWORD_PTR>(nativeMessage->lParam);
            const int globalX = static_cast<short>(LOWORD(lParam));
            const int globalY = static_cast<short>(HIWORD(lParam));
            const QPoint globalPosition(globalX, globalY);
            const QPoint flatPosition = m_flatView->mapFromGlobal(globalPosition);
            if (m_flatView->rect().contains(flatPosition)) {
                const double steps = static_cast<double>(GET_WHEEL_DELTA_WPARAM(nativeMessage->wParam)) / WHEEL_DELTA;
                adjustFlatZoom(steps);
                *result = 0;

                return true;
            }
        }
    }

    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::adjustFlatZoom(const double steps) {
    if (std::abs(steps) < 0.001) {
        return;
    }

    m_flatView->zoom(steps);
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (!m_splittersArranged) {
        arrangeSplitters();
        m_splittersArranged = true;
    }
    QTimer::singleShot(0, this, &MainWindow::updateVolumeSliderPlacement);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    leavePlaytest();
    if (!confirmDiscardUnsavedChanges(QStringLiteral("quitting"))) {
        event->ignore();
        return;
    }
    saveLayout();
    QMainWindow::closeEvent(event);
}

bool MainWindow::confirmDiscardUnsavedChanges(const QString& action) {
    if (!m_hasProject || !m_projectDirty) {
        return true;
    }

    QMessageBox prompt(this);
    prompt.setIcon(QMessageBox::Warning);
    prompt.setWindowTitle(QStringLiteral("Unsaved project"));
    prompt.setText(QStringLiteral("This project has unsaved changes."));
    prompt.setInformativeText(QStringLiteral("Save before %1?").arg(action));
    QPushButton* saveButton = prompt.addButton(QMessageBox::Save);
    QPushButton* discardButton = prompt.addButton(QStringLiteral("Discard"), QMessageBox::DestructiveRole);
    prompt.addButton(QMessageBox::Cancel);
    prompt.setDefaultButton(saveButton);
    prompt.exec();
    if (prompt.clickedButton() == saveButton) {
        saveProject();
        return !m_projectDirty;
    }

    return prompt.clickedButton() == discardButton;
}

void MainWindow::markProjectDirty() {
    if (m_hasProject && !m_loadingProject) {
        m_projectDirty = true;
    }
}

void MainWindow::buildInterface() {
    auto* centralWidget = new QWidget(this);
    auto* layout = new QVBoxLayout(centralWidget);
    m_contentHost = new QWidget(centralWidget);
    m_flatModeSelector = new QComboBox(centralWidget);
    m_noteSpeedSelector = new QDoubleSpinBox(centralWidget);
    m_statusLabel = new QLabel(QStringLiteral("Import an SPC file open or create a project to begin."), centralWidget);
    m_flatView = new FlatView(this);
    m_viewer = new ConveyorView(centralWidget);
    m_timeline = new TimelineWidget(this);
    m_playbackControls = new PlaybackControls(this);
    m_playbackControls->setPlaying(m_playback->isPlaying());
    m_playbackControls->setPlaybackRate(m_playback->playbackRate());
    m_timingPositionLabel = new QLabel(QStringLiteral("00:00:000"), this);

    m_flatModeSelector->addItems({QStringLiteral("Ground"), QStringLiteral("Sky"), QStringLiteral("Both")});
    m_flatModeSelector->setCurrentIndex(2);
    m_noteSpeedSelector->setRange(0.1, 10.0);
    m_noteSpeedSelector->setDecimals(2);
    m_noteSpeedSelector->setSingleStep(0.05);
    m_noteSpeedSelector->setPrefix(QStringLiteral("Conveyor speed "));
    m_noteSpeedSelector->setSuffix(QStringLiteral("x"));
    m_noteSpeedSelector->setToolTip(QStringLiteral("Matches the game's note-speed setting in the 3D view only."));
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    m_noteSpeedSelector->setValue(settings.value(QStringLiteral("viewer/note_speed"), 2.5).toDouble());
    m_viewer->setNoteSpeed(m_noteSpeedSelector->value());
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_contentHost);
    setCentralWidget(centralWidget);
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("File"));
    auto* editMenu = menuBar()->addMenu(QStringLiteral("Edit"));
    m_windowMenu = menuBar()->addMenu(QStringLiteral("Window"));
    auto* settingsMenu = menuBar()->addMenu(QStringLiteral("Settings"));
    auto* chartboxAction = menuBar()->addAction(QStringLiteral("Chartbox"));
    auto* newProjectAction = fileMenu->addAction(QStringLiteral("New project..."));
    auto* openProjectAction = fileMenu->addAction(QStringLiteral("Open project..."));
    m_recentProjectMenu = fileMenu->addMenu(QStringLiteral("Open recent project"));
    auto* importSpcAction = fileMenu->addAction(QStringLiteral("Import SPC..."));
    auto* saveProjectAction = fileMenu->addAction(QStringLiteral("Save project"));
    auto* saveProjectAsAction = fileMenu->addAction(QStringLiteral("Save project as..."));
    auto* exportProjectAction = fileMenu->addAction(QStringLiteral("Export project..."));
    const auto addEditAction = [this, editMenu](const QString& text, const KeyCommand command) {
        QAction* action = editMenu->addAction(text);
        m_keyBindings->registerAction(command, action);

        return action;
    };
    auto* undoAction = addEditAction(QStringLiteral("Undo"), KeyCommand::Undo);
    auto* redoAction = addEditAction(QStringLiteral("Redo"), KeyCommand::Redo);
    editMenu->addSeparator();
    auto* cutAction = addEditAction(QStringLiteral("Cut"), KeyCommand::Cut);
    auto* copyAction = addEditAction(QStringLiteral("Copy"), KeyCommand::Copy);
    auto* pasteAction = addEditAction(QStringLiteral("Paste"), KeyCommand::Paste);
    auto* deleteAction = addEditAction(QStringLiteral("Delete selection"), KeyCommand::DeleteSelection);
    editMenu->addSeparator();
    auto* selectAllAction = addEditAction(QStringLiteral("Select all visible"), KeyCommand::SelectAll);
    auto* mirrorAction = addEditAction(QStringLiteral("Mirror selection"), KeyCommand::MirrorSelection);
    auto* verticalFlipAction = addEditAction(QStringLiteral("Flip selection vertically"), KeyCommand::FlipSelectionVertically);
    auto* groupingAction = addEditAction(QStringLiteral("Group selection"), KeyCommand::ToggleFieldGrouping);
    auto* resnapAllAction = addEditAction(QStringLiteral("Resnap all hit objects"), KeyCommand::ResnapAll);
    auto* verifyAction = addEditAction(QStringLiteral("Refresh verification"), KeyCommand::RefreshVerification);
    auto* resetLayoutAction = m_windowMenu->addAction(QStringLiteral("Reset layout"));
    auto* settingsAction = settingsMenu->addAction(QStringLiteral("Settings..."));
    m_keyBindings->registerAction(KeyCommand::NewProject, newProjectAction);
    m_keyBindings->registerAction(KeyCommand::OpenProject, openProjectAction);
    m_keyBindings->registerAction(KeyCommand::ImportSpc, importSpcAction);
    m_keyBindings->registerAction(KeyCommand::SaveProject, saveProjectAction);
    m_keyBindings->registerAction(KeyCommand::SaveProjectAs, saveProjectAsAction);
    m_keyBindings->registerAction(KeyCommand::ExportProject, exportProjectAction);
    m_keyBindings->registerAction(KeyCommand::ResetLayout, resetLayoutAction);
    m_keyBindings->registerAction(KeyCommand::Settings, settingsAction);
    setWindowTitle(QStringLiteral("In Libertas"));
    resize(1540, 980);

    connect(newProjectAction, &QAction::triggered, this, &MainWindow::createProject);
    connect(openProjectAction, &QAction::triggered, this, &MainWindow::openProject);
    connect(importSpcAction, &QAction::triggered, this, &MainWindow::importSpc);
    connect(saveProjectAction, &QAction::triggered, this, &MainWindow::saveProject);
    connect(saveProjectAsAction, &QAction::triggered, this, &MainWindow::saveProjectAs);
    connect(exportProjectAction, &QAction::triggered, this, &MainWindow::exportProject);
    connect(undoAction, &QAction::triggered, m_state, &EditorState::undo);
    connect(redoAction, &QAction::triggered, m_state, &EditorState::redo);
    connect(cutAction, &QAction::triggered, m_state, &EditorState::cutSelectedHitObjects);
    connect(copyAction, &QAction::triggered, m_state, &EditorState::copySelectedHitObjects);
    connect(pasteAction, &QAction::triggered, m_state, &EditorState::pasteCopiedHitObjects);
    connect(deleteAction, &QAction::triggered, m_state, &EditorState::removeSelection);
    connect(selectAllAction, &QAction::triggered, this, &MainWindow::selectAllVisibleHitObjects);
    connect(mirrorAction, &QAction::triggered, m_state, &EditorState::mirrorSelectedHitObjects);
    connect(verticalFlipAction, &QAction::triggered, m_state, &EditorState::flipSelectedHitObjectsVertically);
    connect(groupingAction, &QAction::triggered, m_state, &EditorState::toggleSelectedFieldGrouping);
    connect(resnapAllAction, &QAction::triggered, m_state, &EditorState::resnapAllHitObjects);
    connect(verifyAction, &QAction::triggered, this, [this] {
        m_verification->setChart(m_state->chart(), m_state->speedEvents(), m_state->timingPoints());
    });
    connect(resetLayoutAction, &QAction::triggered, this, &MainWindow::resetLayout);
    connect(settingsAction, &QAction::triggered, this, &MainWindow::openSettings);
    connect(chartboxAction, &QAction::triggered, this, &MainWindow::openChartbox);
    connect(m_state, &EditorState::historyChanged, this, [undoAction, redoAction](const bool canUndo, const bool canRedo) {
        undoAction->setEnabled(canUndo);
        redoAction->setEnabled(canRedo);
    });
    refreshRecentProjectMenu();
    connect(m_flatModeSelector, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](const int index) {
        m_state->setFlatViewMode(static_cast<FlatViewMode>(index));
    });
    connect(m_noteSpeedSelector, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](const double noteSpeed) {
        m_viewer->setNoteSpeed(noteSpeed);
        QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
        settings.setValue(QStringLiteral("viewer/note_speed"), noteSpeed);
    });
    connect(m_playback, &PlaybackController::positionChanged, this, [this](qint64 position) {
        if (!m_playtest) {
            m_state->setPlaybackPosition(position);
        }
    });
    connect(m_playback, &PlaybackController::audioSourceChanged, m_flatView, &FlatView::clearWaveform);
    connect(m_playback, &PlaybackController::audioDecoded, m_flatView, &FlatView::setDecodedAudio);
    connect(m_playback, &PlaybackController::audioDecoded, this, &MainWindow::analyzeInitialTimingAudio);
    connect(m_playbackControls, &PlaybackControls::togglePlaybackRequested, this, &MainWindow::togglePlayback);
    connect(m_playbackControls, &PlaybackControls::playbackRateRequested, m_playback, &PlaybackController::setPlaybackRate);
    connect(m_playback, &PlaybackController::playbackChanged, m_playbackControls, &PlaybackControls::setPlaying);
    connect(m_playback, &PlaybackController::playbackRateChanged, m_playbackControls, &PlaybackControls::setPlaybackRate);
    connect(m_playback, &PlaybackController::playbackRateChanged, this, [this](const qreal playbackRate) {
        Q_UNUSED(playbackRate)
        updateStatus();
    });
    connect(m_playback, &PlaybackController::audioError, this, [this](const QString& message) {
        if (m_timingAnalysisInProgress) {
            cancelInitialTimingAnalysis();
            m_statusLabel->setText(QStringLiteral("New project created; timing detection failed: %1").arg(message));
            return;
        }
        m_statusLabel->setText(QStringLiteral("Audio fallback clock: %1").arg(message));
    });
    connect(m_flatView, &FlatView::waveformError, this, [this](const QString& message) {
        m_statusLabel->setText(QStringLiteral("Waveform decoding failed: %1").arg(message));
    });
    connect(m_playback, &PlaybackController::mediaDurationChanged, this, &MainWindow::updateProjectDuration);
    connect(m_timeline, &TimelineWidget::playbackPositionRequested, m_state, &EditorState::setPlaybackPosition);
    connect(m_flatView, &FlatView::playbackPositionRequested, m_state, &EditorState::setPlaybackPosition);
    connect(m_flatView, &FlatView::divisorSeekRequested, m_state, &EditorState::seekByDivisor);
    connect(m_flatView, &FlatView::measureSeekRequested, m_state, &EditorState::seekByMeasure);
    connect(m_flatView, &FlatView::divisorIncreaseRequested, m_state, &EditorState::increaseDivisor);
    connect(m_flatView, &FlatView::divisorDecreaseRequested, m_state, &EditorState::decreaseDivisor);
    connect(m_flatView, &FlatView::hitObjectAddRequested, m_state, &EditorState::addHitObject);
    connect(m_flatView, &FlatView::hitObjectRemoveRequested, m_state, &EditorState::removeHitObject);
    connect(m_flatView, &FlatView::hitObjectCreationCancelled, m_state, &EditorState::cancelAddedHitObject);
    connect(m_flatView, &FlatView::hitObjectMoveStarted, m_state, &EditorState::beginHitObjectMove);
    connect(m_flatView, &FlatView::hitObjectMoveRequested, m_state, &EditorState::moveHitObject);
    connect(m_flatView, &FlatView::hitObjectMoveFinished, m_state, &EditorState::finishHitObjectMove);
    connect(m_flatView, &FlatView::hitObjectBatchMoveStarted, m_state, &EditorState::beginHitObjectBatchMove);
    connect(m_flatView, &FlatView::hitObjectBatchMoveRequested, m_state, &EditorState::moveHitObjectBatch);
    connect(m_flatView, &FlatView::hitObjectBatchMoveFinished, m_state, &EditorState::finishHitObjectBatchMove);
    connect(m_flatView, &FlatView::hitObjectSelectionRequested, m_state, &EditorState::setSelectedHitObjects);
    connect(m_flatView, &FlatView::hitObjectsCopyRequested, m_state, &EditorState::copyHitObjects);
    connect(m_state, &EditorState::playbackPositionChanged, m_playback, &PlaybackController::setPosition);
    connect(m_state, &EditorState::chartChanged, this, [this] {
        const ChartData& chart = m_state->chart();
        m_viewer->setChart(chart);
        m_flatView->setChart(chart);
        m_verification->setChart(chart, m_state->speedEvents(), m_state->timingPoints());
        m_timeline->setDuration(chart.durationMilliseconds);
        m_playback->setChartDuration(chart.durationMilliseconds);
        m_playback->setHitObjects(chart);
        if (m_hasProject && !m_loadingProject) {
            storeLoadedDifficulty();
            markProjectDirty();
        }
        updateStatus();
    });
    connect(m_state, &EditorState::playbackPositionChanged, this, [this](const qint64 positionMilliseconds) {
        m_viewer->setPlaybackPosition(positionMilliseconds);
        m_flatView->setPlaybackPosition(positionMilliseconds);
        m_flatView->setGridDuration(m_state->divisorDurationMilliseconds());
        m_timeline->setPlaybackPosition(positionMilliseconds);
        m_timingPositionLabel->setText(formatPreciseTimestamp(positionMilliseconds));
    });
    connect(m_state, &EditorState::flatViewModeChanged, m_flatView, &FlatView::setMode);
    connect(m_state, &EditorState::toolChanged, m_flatView, &FlatView::setTool);
    connect(m_state, &EditorState::divisorChanged, this, [this] {
        m_flatView->setGridDuration(m_state->divisorDurationMilliseconds());
        m_flatView->setDivisor(m_state->divisor());
    });
    connect(m_state, &EditorState::hitObjectAdded, m_viewer, &ConveyorView::addHitObject);
    connect(m_state, &EditorState::hitObjectAdded, m_flatView, &FlatView::addHitObject);
    connect(m_state, &EditorState::hitObjectRemoved, m_viewer, &ConveyorView::removeHitObject);
    connect(m_state, &EditorState::hitObjectRemoved, m_flatView, &FlatView::removeHitObject);
    connect(m_state, &EditorState::hitObjectChanged, m_viewer, &ConveyorView::updateHitObject);
    connect(m_state, &EditorState::hitObjectChanged, m_flatView, &FlatView::updateHitObject);
    connect(m_state, &EditorState::hitObjectChanged, this, [this] {
        QTimer::singleShot(0, m_properties, [this] {
            m_properties->setSelection(*m_state);
        });
    });
    connect(m_state, &EditorState::hitObjectsBatchChanged, m_viewer, &ConveyorView::updateHitObjects);
    connect(m_state, &EditorState::hitObjectsBatchChanged, m_flatView, &FlatView::updateHitObjects);
    connect(m_state, &EditorState::hitObjectsBatchChanged, this, [this] {
        QTimer::singleShot(0, m_properties, [this] {
            m_properties->setSelection(*m_state);
        });
    });
    connect(m_state, &EditorState::hitObjectsChanged, this, [this] {
        m_playback->setHitObjects(m_state->chart());
        m_verification->setChart(m_state->chart(), m_state->speedEvents(), m_state->timingPoints());
        markProjectDirty();
        updateStatus();
    });
    connect(m_state, &EditorState::hitObjectsPasted, this, [this] {
        m_verification->setChart(m_state->chart(), m_state->speedEvents(), m_state->timingPoints());
        markProjectDirty();
    });
}

void MainWindow::arrangeSplitters() {
    m_rootSplitter->setSizes({220, 1320});
    m_mainSplitter->setSizes({580, 980});
    m_viewerSplitter->setSizes({650, 280});
    m_bottomSplitter->setSizes({310, 360, 420});
}

void MainWindow::updateBottomPanelVisibility() {
    m_bottomSplitter->setVisible(std::any_of(m_workspacePanels.cbegin() + 1, m_workspacePanels.cend(),
        [](const QWidget* panel) { return !panel->isHidden(); }));
}

void MainWindow::restoreLayout() {
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    m_flatView->setWaveformWidth(settings.value(QStringLiteral("layout/waveform_width"),
        FlatView::kDefaultWaveformWidth).toDouble());
    restoreGeometry(settings.value(QStringLiteral("layout/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("layout/window_state")).toByteArray());
    const auto restoreSplitter = [&settings](QSplitter* splitter, const QString& key) {
        const QByteArray state = settings.value(key).toByteArray();
        return !state.isEmpty() && splitter->restoreState(state);
    };
    // Do not combine these calls with ||: its short-circuiting would restore
    // the first splitter only and silently skip the lower 3D-view splitter.
    const bool restoredRoot = restoreSplitter(m_rootSplitter, QStringLiteral("layout/root_splitter"));
    const bool restoredMain = restoreSplitter(m_mainSplitter, QStringLiteral("layout/main_splitter"));
    const bool restoredViewer = restoreSplitter(m_viewerSplitter, QStringLiteral("layout/viewer_splitter"));
    const bool restoredBottom = restoreSplitter(m_bottomSplitter, QStringLiteral("layout/bottom_splitter"));
    for (QWidget* panel : m_workspacePanels) {
        panel->setVisible(settings.value(QStringLiteral("layout/panels/%1").arg(panel->windowTitle()), true).toBool());
    }
    updateBottomPanelVisibility();
    const bool restoredSplitters = restoredRoot || restoredMain || restoredViewer || restoredBottom;
    m_splittersArranged = restoredSplitters;
}

void MainWindow::saveLayout() const {
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    settings.setValue(QStringLiteral("layout/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("layout/window_state"), saveState());
    settings.setValue(QStringLiteral("layout/root_splitter"), m_rootSplitter->saveState());
    settings.setValue(QStringLiteral("layout/main_splitter"), m_mainSplitter->saveState());
    settings.setValue(QStringLiteral("layout/viewer_splitter"), m_viewerSplitter->saveState());
    settings.setValue(QStringLiteral("layout/bottom_splitter"), m_bottomSplitter->saveState());
    settings.setValue(QStringLiteral("layout/waveform_width"), m_flatView->waveformWidth());
    for (const QWidget* panel : m_workspacePanels) {
        settings.setValue(QStringLiteral("layout/panels/%1").arg(panel->windowTitle()), !panel->isHidden());
    }
}

void MainWindow::resetLayout() {
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    settings.remove(QStringLiteral("layout"));
    for (QWidget* panel : m_workspacePanels) {
        panel->show();
    }
    updateBottomPanelVisibility();
    m_flatView->setWaveformWidth(FlatView::kDefaultWaveformWidth);
    resize(1540, 980);
    arrangeSplitters();
    m_splittersArranged = true;
}

void MainWindow::selectAllVisibleHitObjects() {
    QVector<int> selectedIndexes;
    const FlatViewMode mode = m_state->flatViewMode();
    for (int index = 0; index < m_state->chart().notes.size(); ++index) {
        const bool sky = isSkyHitObject(m_state->chart().notes.at(index));
        if ((sky && mode != FlatViewMode::Ground) || (!sky && mode != FlatViewMode::Sky)) {
            selectedIndexes.append(index);
        }
    }
    m_state->setSelectedHitObjects(std::move(selectedIndexes));
}

void MainWindow::buildToolBar() {
    m_toolsToolbar = new QToolBar(QStringLiteral("Tools"), this);
    m_toolsToolbar->setObjectName(QStringLiteral("editorToolsToolbar"));
    auto* actions = new QActionGroup(m_toolsToolbar);
    auto* placeAction = m_toolsToolbar->addAction(QStringLiteral("Place"));
    auto* selectAction = m_toolsToolbar->addAction(QStringLiteral("Select"));
    auto* moveAction = m_toolsToolbar->addAction(QStringLiteral("Move"));
    m_keyBindings->registerAction(KeyCommand::PlaceTool, placeAction, false);
    m_keyBindings->registerAction(KeyCommand::SelectTool, selectAction, false);
    m_keyBindings->registerAction(KeyCommand::MoveTool, moveAction, false);
    m_toolsToolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_toolsToolbar->setMovable(false);
    placeAction->setCheckable(true);
    selectAction->setCheckable(true);
    moveAction->setCheckable(true);
    placeAction->setChecked(true);
    actions->setExclusive(true);
    actions->addAction(placeAction);
    actions->addAction(selectAction);
    actions->addAction(moveAction);
    m_toolsToolbar->setMinimumWidth(kToolsToolbarMinimumWidth);
    m_volumeSlider = new QSlider(Qt::Vertical, m_toolsToolbar);
    m_volumeSlider->setObjectName(QStringLiteral("volumeSlider"));
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setInvertedAppearance(false);
    m_volumeSlider->setAccessibleName(QStringLiteral("Song volume"));
    m_volumeSlider->setStyleSheet(QStringLiteral(R"(
        QSlider#volumeSlider {
            background: rgb(3, 11, 18);
            border: 1px solid rgb(0, 0, 0);
            padding: 2px 1px;
        }
        QSlider#volumeSlider::groove:vertical {
            background: rgb(6, 17, 27);
            border: 1px solid rgb(0, 8, 13);
            width: 30px;
            margin: 0;
        }
        QSlider#volumeSlider::add-page:vertical {
            background: rgb(0, 167, 212);
        }
        QSlider#volumeSlider::sub-page:vertical {
            background: rgb(6, 17, 27);
        }
        QSlider#volumeSlider::handle:vertical {
            background: rgb(191, 244, 244);
            border: 1px solid rgb(219, 255, 255);
            height: 5px;
            margin: 0 -2px;
        }
    )"));
    m_effectsVolumeSlider = new QSlider(Qt::Vertical, m_toolsToolbar);
    m_effectsVolumeSlider->setObjectName(QStringLiteral("effectsVolumeSlider"));
    m_effectsVolumeSlider->setRange(m_volumeSlider->minimum(), m_volumeSlider->maximum());
    m_effectsVolumeSlider->setAccessibleName(QStringLiteral("Hitsound volume"));
    m_effectsVolumeSlider->setStyleSheet(QString(m_volumeSlider->styleSheet()).replace(
        QStringLiteral("volumeSlider"), QStringLiteral("effectsVolumeSlider")));
    m_songVolumeLabel = new QLabel(QStringLiteral("SNG"), m_toolsToolbar);
    m_effectsVolumeLabel = new QLabel(QStringLiteral("SFX"), m_toolsToolbar);
    m_songVolumeLabel->setAlignment(Qt::AlignCenter);
    m_effectsVolumeLabel->setAlignment(Qt::AlignCenter);

    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    const int volumePercent = std::clamp(settings.value(QStringLiteral("playback/volume"), 100).toInt(), 0, 100);
    const int effectsVolumePercent = std::clamp(settings.value(QStringLiteral("playback/effects_volume"), 100).toInt(), 0, 100);
    m_volumeSlider->setValue(volumePercent);
    m_volumeSlider->setToolTip(QStringLiteral("Song volume: %1%").arg(volumePercent));
    m_playback->setVolume(logarithmicVolumeForPercent(volumePercent));
    m_effectsVolumeSlider->setValue(effectsVolumePercent);
    m_effectsVolumeSlider->setToolTip(QStringLiteral("Hitsound volume: %1%").arg(effectsVolumePercent));
    m_playback->setEffectsVolume(logarithmicVolumeForPercent(effectsVolumePercent));
    addToolBar(Qt::LeftToolBarArea, m_toolsToolbar);

    connect(placeAction, &QAction::triggered, this, [this] { m_state->setTool(EditorTool::Place); });
    connect(selectAction, &QAction::triggered, this, [this] { m_state->setTool(EditorTool::Select); });
    connect(moveAction, &QAction::triggered, this, [this] { m_state->setTool(EditorTool::Move); });
    connect(m_state, &EditorState::toolChanged, this, [placeAction, selectAction, moveAction](const EditorTool tool) {
        placeAction->setChecked(tool == EditorTool::Place);
        selectAction->setChecked(tool == EditorTool::Select);
        moveAction->setChecked(tool == EditorTool::Move);
    });
    connect(m_volumeSlider, &QSlider::valueChanged, this, [this](const int volume) {
        m_playback->setVolume(logarithmicVolumeForPercent(volume));
        m_volumeSlider->setToolTip(QStringLiteral("Song volume: %1%").arg(volume));
        QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
        settings.setValue(QStringLiteral("playback/volume"), volume);
    });
    connect(m_effectsVolumeSlider, &QSlider::valueChanged, this, [this](const int volume) {
        m_playback->setEffectsVolume(logarithmicVolumeForPercent(volume));
        m_effectsVolumeSlider->setToolTip(QStringLiteral("Hitsound volume: %1%").arg(volume));
        QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
        settings.setValue(QStringLiteral("playback/effects_volume"), volume);
    });
}

void MainWindow::updateToolButtonSizes() {
    const QMargins margins = m_toolsToolbar->layout()->contentsMargins();
    const auto actions = m_toolsToolbar->actions();
    QFont font = m_toolsToolbar->font();
    const int buttonSize = std::max(1, m_toolsToolbar->width() - margins.left() - margins.right());
    const int textSize = std::max(1, buttonSize - kToolButtonTextPadding * 2);
    int minimumSize = 1;
    int maximumSize = textSize;

    while (minimumSize < maximumSize) {
        const int candidateSize = (minimumSize + maximumSize + 1) / 2;
        font.setPixelSize(candidateSize);
        const QFontMetrics metrics(font);
        const bool fits = metrics.height() <= textSize
            && std::all_of(actions.cbegin(), actions.cend(),
                [&metrics, textSize](const QAction* action) { return metrics.horizontalAdvance(action->text()) <= textSize; });
        if (fits) {
            minimumSize = candidateSize;
        } else {
            maximumSize = candidateSize - 1;
        }
    }
    font.setPixelSize(minimumSize);
    for (QAction* action : actions) {
        auto* button = qobject_cast<QToolButton*>(m_toolsToolbar->widgetForAction(action));
        if (button == nullptr) {
            continue;
        }
        if (button->minimumSize() != QSize(buttonSize, buttonSize) || button->maximumSize() != QSize(buttonSize, buttonSize)) {
            button->setFixedSize(buttonSize, buttonSize);
        }
        if (button->font() != font) {
            button->setFont(font);
        }
    }
    m_toolsToolbar->layout()->activate();
}

void MainWindow::registerNavigationKeybinds() {
    const auto addAction = [this](const KeyCommand command, const auto& handler) {
        auto* action = new QAction(this);
        m_keyBindings->registerAction(command, action, false);
        connect(action, &QAction::triggered, this, handler);
    };
    addAction(KeyCommand::ZoomIn, [this] { adjustFlatZoom(1.0); });
    addAction(KeyCommand::ZoomInAlternate, [this] { adjustFlatZoom(1.0); });
    addAction(KeyCommand::ZoomOut, [this] { adjustFlatZoom(-1.0); });
    addAction(KeyCommand::ZoomOutAlternate, [this] { adjustFlatZoom(-1.0); });
    addAction(KeyCommand::AddTimingPoint, [this] { m_state->addTimingPointAtPlaybackPosition(); });
    addAction(KeyCommand::AddSpeedEvent, [this] { m_state->addSpeedEventAtPlaybackPosition(); });
    addAction(KeyCommand::TogglePlayback, [this] { togglePlayback(); });
    addAction(KeyCommand::EnterPlaytest, [this] { enterPlaytest(); });
    addAction(KeyCommand::IncreasePlaybackRate, [this] { m_playback->increasePlaybackRate(); });
    addAction(KeyCommand::DecreasePlaybackRate, [this] { m_playback->decreasePlaybackRate(); });
    addAction(KeyCommand::SeekBackward, [this] { m_state->seekByDivisor(-1); });
    addAction(KeyCommand::SeekForward, [this] { m_state->seekByDivisor(1); });
    addAction(KeyCommand::IncreaseDivisor, [this] { m_state->increaseDivisor(); });
    addAction(KeyCommand::DecreaseDivisor, [this] { m_state->decreaseDivisor(); });
    addAction(KeyCommand::CycleFlatMode, [this] {
        m_flatModeSelector->setCurrentIndex((m_flatModeSelector->currentIndex() + 1) % m_flatModeSelector->count());
    });
    addAction(KeyCommand::ToggleSkyGrid, [this] { m_skyGridToggle->toggle(); });
    addAction(KeyCommand::SkyGridSize1, [this] { setSkyGridSize(1); });
    addAction(KeyCommand::SkyGridSize2, [this] { setSkyGridSize(2); });
    addAction(KeyCommand::SkyGridSize3, [this] { setSkyGridSize(3); });
    addAction(KeyCommand::SkyGridSize4, [this] { setSkyGridSize(4); });
}

void MainWindow::setSkyGridSize(const int divisor) {
    m_skyGridSizes->button(divisor)->setChecked(true);
    m_flatView->setSkyGridDivisor(divisor);
}

void MainWindow::openSettings() {
    SettingsDialog dialog(m_keyBindings, m_visualSettings, m_handlingSettings, m_playtestSettings,
        m_hitsounds, m_playback, this);
    if (dialog.exec() == QDialog::Accepted) {
        m_hitsounds->load(HitsoundBank::loadFiles());
        m_visualSettings = dialog.selectedVisualSettings();
        m_flatView->setPlayheadColor(m_visualSettings.effectivePlayheadColor());
        m_handlingSettings = dialog.selectedHandlingSettings();
        m_flatView->setInvertMousewheelScroll(m_handlingSettings.invertMousewheelScroll);
        m_playtestSettings = dialog.selectedPlaytestSettings();
        m_viewer->setPlaytestLaneKeys(m_playtestSettings.laneKeys);
    }
}

void MainWindow::openChartbox() {
    ChartboxDialog dialog(m_hasProject ? m_project.chartboxId : QString(), m_hasProject, this);
    if (m_hasProject) {
        dialog.setUploadHandlers([this] {
            return saveProject() && !m_projectPath.isEmpty() ? m_projectPath : QString();
        }, [this](const QString& reference) {
            m_project.chartboxId = reference;
            markProjectDirty();
            return saveProject();
        });
    }
    dialog.exec();
}

void MainWindow::updateVolumeSliderPlacement() {
    if (m_toolsToolbar == nullptr || m_volumeSlider == nullptr
        || !m_toolsToolbar->isVisible() || !m_flatView->isVisible()) {
        return;
    }
    updateToolButtonSizes();

    constexpr int kHorizontalPadding = 7;
    constexpr int kBottomPadding = 8;
    constexpr int kColumnSpacing = 6;
    constexpr int kLabelHeight = 20;
    int minimumTop = kLabelHeight;
    for (QAction* action : m_toolsToolbar->actions()) {
        minimumTop = std::max(minimumTop, m_toolsToolbar->actionGeometry(action).bottom() + kColumnSpacing + kLabelHeight);
    }
    const int desiredTop = m_toolsToolbar->mapFromGlobal(
        m_flatView->mapToGlobal(QPoint(0, m_flatView->playheadViewportY()))).y();
    const int sliderTop = std::clamp(desiredTop, minimumTop,
        std::max(minimumTop, m_toolsToolbar->height() - kBottomPadding));
    const int sliderHeight = std::max(0, m_toolsToolbar->height() - sliderTop - kBottomPadding);
    const int sliderWidth = std::max(0, (m_toolsToolbar->width() - kHorizontalPadding * 2 - kColumnSpacing) / 2);
    const int effectsLeft = kHorizontalPadding + sliderWidth + kColumnSpacing;
    m_volumeSlider->setGeometry(kHorizontalPadding, sliderTop, sliderWidth, sliderHeight);
    m_effectsVolumeSlider->setGeometry(effectsLeft, sliderTop, sliderWidth, sliderHeight);
    m_songVolumeLabel->setGeometry(kHorizontalPadding, sliderTop - kLabelHeight, sliderWidth, kLabelHeight);
    m_effectsVolumeLabel->setGeometry(effectsLeft, sliderTop - kLabelHeight, sliderWidth, kLabelHeight);
    m_volumeSlider->raise();
    m_effectsVolumeSlider->raise();
    m_songVolumeLabel->raise();
    m_effectsVolumeLabel->raise();
}

void MainWindow::buildWorkspace() {
    m_metadata = new MetadataWidget(this);
    m_timing = new EventsWidget(this);
    m_properties = new PropertiesPanel(this);
    m_verification = new VerificationWidget(this);
    auto* timingHost = new QWidget(this);
    auto* timingLayout = new QVBoxLayout(timingHost);
    m_verification->setMinimumSize(260, 180);
    m_rootSplitter = new QSplitter(Qt::Horizontal, m_contentHost);
    m_mainSplitter = new QSplitter(Qt::Horizontal, m_rootSplitter);
    m_viewerSplitter = new QSplitter(Qt::Vertical, m_mainSplitter);
    m_bottomSplitter = new QSplitter(Qt::Horizontal, m_viewerSplitter);
    m_timingPositionLabel->setAlignment(Qt::AlignCenter);
    QFont timingPositionFont = m_timingPositionLabel->font();
    timingPositionFont.setPointSize(34);
    m_timingPositionLabel->setFont(timingPositionFont);
    m_timingPositionLabel->setFixedHeight(TimelineWidget::kWidgetHeight);
    timingLayout->setContentsMargins(0, 0, 0, 0);
    timingLayout->setSpacing(2);
    timingLayout->addWidget(m_timing, 1);
    timingLayout->addWidget(m_timingPositionLabel);
    auto* timingPanel = createPanel(QStringLiteral("Events"), timingHost, m_rootSplitter);
    auto* flatControls = new QWidget(this);
    auto* flatControlsLayout = new QHBoxLayout(flatControls);
    flatControlsLayout->setContentsMargins(0, 0, 0, 0);
    flatControlsLayout->setSpacing(3);
    m_skyGridToggle = new QToolButton(flatControls);
    m_skyGridToggle->setText(QStringLiteral("Grid OFF"));
    m_skyGridToggle->setCheckable(true);
    m_skyGridToggle->setFocusPolicy(Qt::NoFocus);
    m_skyGridToggle->setToolTip(QStringLiteral("Toggle the sky snap grid"));
    flatControlsLayout->addWidget(m_skyGridToggle);
    connect(m_skyGridToggle, &QToolButton::toggled, this, [this](const bool enabled) {
        m_skyGridToggle->setText(enabled ? QStringLiteral("Grid ON") : QStringLiteral("Grid OFF"));
        m_flatView->setSkyGridEnabled(enabled);
    });
    m_skyGridSizes = new QButtonGroup(flatControls);
    m_skyGridSizes->setExclusive(true);
    for (int divisor = 1; divisor <= 4; ++divisor) {
        auto* button = new QToolButton(flatControls);
        button->setText(QStringLiteral("1/%1").arg(divisor));
        button->setCheckable(true);
        button->setChecked(divisor == 1);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(QStringLiteral("Sky grid: %1 subdivisions per lane").arg(divisor));
        m_skyGridSizes->addButton(button, divisor);
        flatControlsLayout->addWidget(button);
    }
    connect(m_skyGridSizes, &QButtonGroup::idClicked, this, &MainWindow::setSkyGridSize);
    flatControlsLayout->addWidget(m_flatModeSelector);
    auto* flatPanel = createPanel(QStringLiteral("Flat View"), m_flatView, m_mainSplitter, flatControls);
    auto* viewerPanel = createPanel(QStringLiteral("3D View"), m_viewer, m_viewerSplitter, m_noteSpeedSelector);
    auto* propertiesPanel = createPanel(QStringLiteral("Properties"), m_properties, m_bottomSplitter);
    auto* metadataPanel = createPanel(QStringLiteral("Metadata"), m_metadata, m_bottomSplitter);
    auto* verificationPanel = createPanel(QStringLiteral("Verification"), m_verification, m_bottomSplitter);
    auto* rightHost = new QWidget(m_rootSplitter);
    auto* rightLayout = new QVBoxLayout(rightHost);
    auto* timelineHost = new QWidget(rightHost);
    auto* timelineLayout = new QHBoxLayout(timelineHost);
    auto* playbackDivider = new QFrame(timelineHost);
    auto* contentLayout = new QHBoxLayout(m_contentHost);

    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(2);
    rightLayout->addWidget(m_mainSplitter, 1);
    timelineLayout->setContentsMargins(0, 0, 0, 0);
    timelineLayout->setSpacing(2);
    timelineLayout->addWidget(m_timeline, 1);
    playbackDivider->setFrameShape(QFrame::VLine);
    playbackDivider->setFrameShadow(QFrame::Sunken);
    timelineLayout->addWidget(playbackDivider);
    timelineLayout->addWidget(m_playbackControls);
    rightLayout->addWidget(timelineHost);
    rightLayout->addWidget(m_statusLabel);
    m_bottomSplitter->addWidget(propertiesPanel);
    m_bottomSplitter->addWidget(metadataPanel);
    m_bottomSplitter->addWidget(verificationPanel);
    m_viewerSplitter->addWidget(viewerPanel);
    m_viewerSplitter->addWidget(m_bottomSplitter);
    m_mainSplitter->addWidget(flatPanel);
    m_mainSplitter->addWidget(m_viewerSplitter);
    m_rootSplitter->addWidget(timingPanel);
    m_rootSplitter->addWidget(rightHost);
    m_rootSplitter->setChildrenCollapsible(false);
    m_mainSplitter->setChildrenCollapsible(false);
    m_viewerSplitter->setChildrenCollapsible(false);
    m_bottomSplitter->setChildrenCollapsible(false);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->addWidget(m_rootSplitter);

    m_workspacePanels = {timingPanel, propertiesPanel, metadataPanel, verificationPanel};
    QAction* resetLayoutAction = m_windowMenu->actions().front();
    for (QWidget* panel : m_workspacePanels) {
        auto* action = new QAction(panel->windowTitle(), m_windowMenu);
        action->setCheckable(true);
        action->setChecked(true);
        m_windowMenu->insertAction(resetLayoutAction, action);
        connect(action, &QAction::triggered, panel, [this, panel](const bool visible) {
            panel->setVisible(visible);
            updateBottomPanelVisibility();
        });
        connect(m_windowMenu, &QMenu::aboutToShow, action, [action, panel] {
            action->setChecked(!panel->isHidden());
        });
    }
    m_windowMenu->insertSeparator(resetLayoutAction);

    connect(m_metadata, &MetadataWidget::metadataEdited, m_state, &EditorState::setMetadata);
    connect(m_metadata, &MetadataWidget::difficultyMetadataEdited, this, [this](DifficultyMetadata metadata) {
        if (!m_hasProject || m_loadingProject) {
            return;
        }
        m_project.difficulties.at(difficultyIndex(m_loadedDifficulty)).metadata = std::move(metadata);
        updateViewerSongCard();
        markProjectDirty();
    });
    connect(m_metadata, &MetadataWidget::chartIdEdited, this, [this](const QString& chartId) {
        if (m_hasProject && !m_loadingProject) {
            m_project.chartId = chartId;
            markProjectDirty();
        }
    });
    connect(m_metadata, &MetadataWidget::jacketRequested, this, [this] {
        if (!m_hasProject) {
            return;
        }
        const QString jacketPath = QFileDialog::getOpenFileName(this, QStringLiteral("Choose jacket"),
            m_project.jacketPath.isEmpty() ? QFileInfo(m_project.songPath).absolutePath() : m_project.jacketPath,
            imageFileDialogFilter());
        if (jacketPath.isEmpty()) {
            return;
        }
        QString error;
        QImage jacket;
        if (!readJacketImage(jacketPath, &jacket, &error)) {
            QMessageBox::warning(this, QStringLiteral("Choose jacket"), error);
            return;
        }
        const QByteArray normalized = encodeJacketImage(jacket, &error);
        if (normalized.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Choose jacket"), error);
            return;
        }
        m_projectJacketData = normalized;
        m_projectJacketImage = std::move(jacket);
        m_project.jacketPath = QStringLiteral("jacket.webp");
        m_metadata->setJacketAvailable(true);
        updateViewerSongCard();
        markProjectDirty();
    });
    connect(m_metadata, &MetadataWidget::previewStartCurrentTimeRequested, this, [this] {
        m_metadata->setPreviewStartMilliseconds(m_state->playbackPosition());
    });
    connect(m_metadata, &MetadataWidget::previewEndCurrentTimeRequested, this, [this] {
        m_metadata->setPreviewEndMilliseconds(m_state->playbackPosition());
    });
    connect(m_metadata, &MetadataWidget::difficultyRequested, m_state, &EditorState::setDifficulty);
    connect(m_properties, &PropertiesPanel::hitObjectsEditRequested, m_state, &EditorState::editHitObjects);
    connect(m_properties, &PropertiesPanel::eventsEditRequested, m_state, &EditorState::editSelectedEvents);
    connect(m_properties, &PropertiesPanel::eventOffsetCurrentTimeRequested, this, [this] {
        m_properties->setEventOffset(m_state->playbackPosition());
    });
    connect(m_timing, &EventsWidget::addTimingRequested, m_state, &EditorState::addTimingPointAtPlaybackPosition);
    connect(m_timing, &EventsWidget::addLaneRequested, m_state, &EditorState::addLaneEventAtPlaybackPosition);
    connect(m_timing, &EventsWidget::addSpeedRequested, m_state, &EditorState::addSpeedEventAtPlaybackPosition);
    connect(m_timing, &EventsWidget::eventSelected, this, [this](const EventsWidget::EntryType type, const int index) {
        m_selectingEventInWidget = true;
        if (type == EventsWidget::EntryType::Timing) {
            m_state->setSelectedTimingPoint(index);
        } else if (type == EventsWidget::EntryType::Lane) {
            m_state->setSelectedLaneEvent(index);
        } else {
            m_state->setSelectedSpeedEvent(index);
        }
        m_selectingEventInWidget = false;
    });
    connect(m_timing, &EventsWidget::timingEventsSelected, this, [this](QVector<int> indexes) {
        m_selectingEventInWidget = true;
        m_state->setSelectedTimingPoints(std::move(indexes));
        m_selectingEventInWidget = false;
    });
    connect(m_timing, &EventsWidget::eventsSelected, this, [this](QVector<int> timingIndexes, QVector<int> laneIndexes,
        QVector<int> speedIndexes) {
        m_selectingEventInWidget = true;
        m_state->setSelectedEvents(std::move(timingIndexes), std::move(laneIndexes), std::move(speedIndexes));
        m_selectingEventInWidget = false;
    });
    connect(m_timing, &EventsWidget::eventSeekRequested, this, [this](const EventsWidget::EntryType type, const int index) {
        const qint64 time = type == EventsWidget::EntryType::Timing
            && index >= 0 && index < m_state->timingPoints().size()
                ? m_state->timingPoints().at(index).timeMilliseconds
                : type == EventsWidget::EntryType::Lane && index >= 0 && index < m_state->laneEvents().size()
                    ? m_state->laneEvents().at(index).timeMilliseconds
                    : type == EventsWidget::EntryType::Speed && index >= 0 && index < m_state->speedEvents().size()
                        ? m_state->speedEvents().at(index).timeMilliseconds
                        : -1;
        if (time >= 0) {
            m_state->setPlaybackPosition(time);
        }
    });
    connect(m_verification, &VerificationWidget::timestampRequested, m_state, &EditorState::setPlaybackPosition);
    connect(m_state, &EditorState::metadataChanged, this, [this] {
        m_metadata->setMetadata(m_state->metadata());
        updateViewerSongCard();
        if (m_hasProject && !m_loadingProject) {
            storeLoadedDifficulty();
            markProjectDirty();
        }
    });
    connect(m_state, &EditorState::difficultyChanged, this, [this](const Difficulty difficulty) {
        m_metadata->setDifficulty(difficulty);
        if (m_hasProject && !m_loadingProject) {
            storeLoadedDifficulty();
            m_loadedDifficulty = difficulty;
            loadDifficulty(difficulty);
        }
        updateViewerSongCard();
        updateStatus();
    });
    connect(m_state, &EditorState::timingPointsChanged, this, [this] {
        m_timing->setEvents(m_state->timingPoints(), m_state->laneEvents(), m_state->speedEvents());
        m_timing->setSelectedEvents(m_state->selectedTimingPoints(), m_state->selectedLaneEvents(),
            m_state->selectedSpeedEvents());
        m_timeline->setTimingPoints(m_state->timingPoints());
        m_flatView->setTimingPoints(m_state->timingPoints());
        m_viewer->setTimingPoints(m_state->timingPoints());
        m_verification->setChart(m_state->chart(), m_state->speedEvents(), m_state->timingPoints());
        if (m_hasProject && !m_loadingProject) {
            storeLoadedDifficulty();
            markProjectDirty();
        }
        QTimer::singleShot(0, m_properties, [this] {
            m_properties->setSelection(*m_state);
        });
    });
    connect(m_state, &EditorState::laneEventsChanged, this, [this] {
        m_timing->setEvents(m_state->timingPoints(), m_state->laneEvents(), m_state->speedEvents());
        m_timing->setSelectedEvents(m_state->selectedTimingPoints(), m_state->selectedLaneEvents(),
            m_state->selectedSpeedEvents());
        m_viewer->setLaneEvents(m_state->laneEvents());
        m_flatView->setLaneEvents(m_state->laneEvents());
        if (m_hasProject && !m_loadingProject) {
            storeLoadedDifficulty();
            markProjectDirty();
        }
        QTimer::singleShot(0, m_properties, [this] {
            m_properties->setSelection(*m_state);
        });
    });
    connect(m_state, &EditorState::speedEventsChanged, this, [this] {
        m_timing->setEvents(m_state->timingPoints(), m_state->laneEvents(), m_state->speedEvents());
        m_timing->setSelectedEvents(m_state->selectedTimingPoints(), m_state->selectedLaneEvents(),
            m_state->selectedSpeedEvents());
        m_timeline->setSpeedEvents(m_state->speedEvents());
        m_flatView->setSpeedEvents(m_state->speedEvents());
        m_viewer->setSpeedEvents(m_state->speedEvents());
        m_verification->setChart(m_state->chart(), m_state->speedEvents(), m_state->timingPoints());
        if (m_hasProject && !m_loadingProject) {
            storeLoadedDifficulty();
            markProjectDirty();
        }
        QTimer::singleShot(0, m_properties, [this] {
            m_properties->setSelection(*m_state);
        });
    });
    connect(m_state, &EditorState::lastTimingPointRemovalRejected, this, [this] {
        QMessageBox::warning(this, QStringLiteral("Cannot delete last timing event"),
            QStringLiteral("Cannot delete last timing event, change its properties istead"));
    });
    connect(m_state, &EditorState::eventPlacementRejected, this, [this](const EventCollision& collision) {
        QMessageBox::warning(this, QStringLiteral("Event already exists"),
            QStringLiteral("[%1] already has an event of type [%2], edit existing instead of placing new one")
                .arg(formatPreciseTimestamp(collision.timeMilliseconds), collision.type));
    });
    connect(m_state, &EditorState::eventPasteCollisionsFound, this, [this](const QVector<EventCollision>& collisions) {
        QStringList messages;
        for (const EventCollision& collision : collisions) {
            messages.append(QStringLiteral("Could not place event [%1] at [%2] - already exists.")
                .arg(collision.type, formatPreciseTimestamp(collision.timeMilliseconds)));
        }
        QMessageBox::warning(this, QStringLiteral("Event paste collisions"), messages.join(QLatin1Char('\n')));
    });
    connect(m_state, &EditorState::toolChanged, this, &MainWindow::updateStatus);
    connect(m_state, &EditorState::divisorChanged, this, &MainWindow::updateStatus);
    connect(m_state, &EditorState::selectionChanged, this, [this] {
        m_flatView->setSelectedHitObjects(m_state->selectedHitObjects());
        if (!m_selectingEventInWidget) {
            m_timing->setSelectedEvents(m_state->selectedTimingPoints(), m_state->selectedLaneEvents(),
                m_state->selectedSpeedEvents());
        }
        QTimer::singleShot(0, m_properties, [this] {
            m_properties->setSelection(*m_state);
        });
    });

    m_metadata->setMetadata(m_state->metadata());
    m_metadata->setDifficulty(m_state->difficulty());
    m_metadata->setDifficultyMetadata({});
    m_metadata->setJacketAvailable(false);
    m_metadata->setJacketEditingEnabled(false);
    m_timing->setEvents(m_state->timingPoints(), m_state->laneEvents(), m_state->speedEvents());
    m_timeline->setTimingPoints(m_state->timingPoints());
    m_timeline->setSpeedEvents(m_state->speedEvents());
    m_flatView->setTimingPoints(m_state->timingPoints());
    m_flatView->setSpeedEvents(m_state->speedEvents());
    m_flatView->setLaneEvents(m_state->laneEvents());
    m_viewer->setTimingPoints(m_state->timingPoints());
    m_viewer->setLaneEvents(m_state->laneEvents());
    m_viewer->setSpeedEvents(m_state->speedEvents());
    m_flatView->setDivisor(m_state->divisor());
    m_properties->setSelection(*m_state);
}

void MainWindow::importSpc() {
    if (!confirmDiscardUnsavedChanges(QStringLiteral("importing an SPC"))) {
        return;
    }
    const QString chartPath = QFileDialog::getOpenFileName(this, QStringLiteral("Import SPC"), m_folderPath,
        QStringLiteral("In Falsus chart (*.spc);;All files (*)"));
    if (!chartPath.isEmpty()) {
        loadChartFile(chartPath);
    }
}

void MainWindow::loadChartFile(const QString& chartPath) {
    const ChartDocument::LoadResult result = ChartDocument::load(chartPath);
    if (!result.succeeded()) {
        m_statusLabel->setText(result.error);
        return;
    }

    const QFileInfo chartFileInfo(chartPath);
    const ImportedSpcIdentity identity = identityForImportedSpc(chartFileInfo);
    m_folderPath = chartFileInfo.absolutePath();
    const QString audioPath = audioPathForFolder(m_folderPath);
    m_projectSongData.clear();
    m_projectJacketData.clear();
    m_projectJacketImage = {};
    cancelInitialTimingAnalysis();
    m_project = blankProject(identity.songName);
    m_project.songPath = audioPath;
    m_project.jacketPath = jacketPathForImportedSpc(chartFileInfo, identity.songName);
    DifficultyChart& importedDifficulty = m_project.difficulties.at(difficultyIndex(identity.difficulty));
    importedDifficulty.hitObjects = result.chart;
    importedDifficulty.timingPoints = result.timingPoints;
    importedDifficulty.laneEvents = result.laneEvents;
    importedDifficulty.speedEvents = result.speedEvents;
    m_projectPath.clear();
    m_hasProject = true;
    m_loadedDifficulty = identity.difficulty;
    m_loadingProject = true;
    m_metadata->setChartId({});
    m_metadata->setJacketAvailable(!m_project.jacketPath.isEmpty());
    m_metadata->setJacketEditingEnabled(true);
    m_metadata->setDifficultyMetadata(importedDifficulty.metadata);
    m_state->setMetadata(m_project.metadata);
    m_state->setDifficulty(identity.difficulty);
    m_state->setChart(importedDifficulty.hitObjects);
    m_state->setTimingPoints(importedDifficulty.timingPoints);
    m_state->setLaneEvents(importedDifficulty.laneEvents);
    m_state->setSpeedEvents(importedDifficulty.speedEvents);
    m_state->setPlaybackPosition(0);
    m_loadingProject = false;
    m_projectDirty = true;
    m_playback->setAudioSource(audioPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(audioPath));
    updateViewerSongCard();
    m_statusLabel->setText(QStringLiteral("Imported %1 as a new unsaved project.").arg(chartFileInfo.fileName()));
}

bool MainWindow::readProjectJacket(QImage* image, QString* error) const {
    if (!m_projectJacketData.isEmpty()) {
        if (m_projectJacketImage.isNull()) {
            *error = QStringLiteral("The jacket embedded in the project cannot be decoded.");
            return false;
        }
        *image = m_projectJacketImage;
        return true;
    }
    if (m_project.jacketPath.isEmpty()) {
        *error = QStringLiteral("Add a valid jacket before exporting.");
        return false;
    }

    return readJacketImage(m_project.jacketPath, image, error);
}

void MainWindow::updateViewerSongCard() {
    if (!m_hasProject) {
        m_viewer->setSongCard({}, Difficulty::Minimal, {}, {});
        return;
    }

    const Difficulty difficulty = m_state->difficulty();
    const DifficultyChart& chart = m_project.difficulties.at(difficultyIndex(difficulty));
    QImage jacket;
    QString error;
    if (!readProjectJacket(&jacket, &error)) {
        jacket = {};
    }
    m_viewer->setSongCard(m_state->metadata(), difficulty, chart.metadata, std::move(jacket));
}

void MainWindow::updateStatus() {
    const QString toolName = [this] {
        switch (m_state->tool()) {
        case EditorTool::Place:
            return QStringLiteral("Place");
        case EditorTool::Select:
            return QStringLiteral("Select");
        case EditorTool::Move:
            return QStringLiteral("Move");
        }

        return QString();
    }();
    if (m_timingAnalysisInProgress) {
        m_statusLabel->setText(QStringLiteral("Detecting initial BPM and offset..."));
        return;
    }
    m_statusLabel->setText(QStringLiteral("%1 · %2 hit objects · %3 · grid 1/%4 · %5%")
        .arg(m_state->chart().name)
        .arg(m_state->chart().notes.size())
        .arg(toolName)
        .arg(m_state->divisor())
        .arg(qRound(m_playback->playbackRate() * 100.0)));
}

void MainWindow::togglePlayback() {
    m_playback->togglePlayback();
}

void MainWindow::enterPlaytest() {
    if (m_playtest || m_timingAnalysisInProgress || m_state->chart().durationMilliseconds <= 0) {
        return;
    }
    if (m_playback->isPlaying()) {
        m_playback->togglePlayback();
    }
    m_editorPosition = m_playback->position();
    m_editorFocus = QApplication::focusWidget();
    m_editorCursor = QCursor::pos();
    m_editorMenuVisible = menuBar()->isVisible();
    m_editorToolsVisible = m_toolsToolbar->isVisible();
    m_playtestWindowState = saveState();
    const std::array<QSplitter*, 4> splitters{m_rootSplitter, m_mainSplitter, m_viewerSplitter, m_bottomSplitter};
    for (int index = 0; index < 4; ++index) {
        m_playtestSplitters[index] = splitters[index]->saveState();
    }
    m_viewerEditorParent = m_viewer->parentWidget();
    m_viewerLayoutIndex = m_viewerEditorParent->layout()->indexOf(m_viewer);
    centralWidget()->hide();
    menuBar()->hide();
    m_toolsToolbar->hide();
    m_viewer->setParent(this);
    m_viewer->setGeometry(rect());
    m_viewer->setFocusPolicy(Qt::StrongFocus);
    m_viewer->setCursor(Qt::BlankCursor);
    m_viewer->show();
    m_viewer->raise();
    m_viewer->setFocus(Qt::OtherFocusReason);
    m_playtestLeadIn = m_editorPosition == 0;
    m_playtestFinished = false;
    m_fieldContact = false;
    m_playtestSession.load(m_state->chart(), m_state->timingPoints(), m_state->speedEvents(),
        m_playtestLeadIn ? -3000.0 : static_cast<double>(m_editorPosition));
    m_playtest = true;
    m_playback->setManualPlaytest(true);
    m_playback->setChartDuration(static_cast<qint64>(std::ceil(m_playtestSession.endTime())) + 2001);
    m_playback->setPosition(m_editorPosition);
    m_leadInEnd = m_playback->sampleClock().monotonicSeconds + 3.0;
    RAWINPUTDEVICE mouse{0x01, 0x02, 0, reinterpret_cast<HWND>(winId())};
    if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) {
        leavePlaytest();
        QMessageBox::warning(this, QStringLiteral("Playtest"), QStringLiteral("Could not capture relative mouse input."));
        return;
    }
    constrainPlaytestCursor();
    if (!m_playtestLeadIn) {
        m_playback->togglePlayback();
    }
    m_playtestTimer->start();
    updatePlaytest();
}

void MainWindow::leavePlaytest() {
    if (!m_playtest) {
        return;
    }
    m_playtestTimer->stop();
    m_playtestSession.clearInput(m_playback->sampleClock().monotonicSeconds);
    m_playback->setHeldHitsounds(false, false);
    if (m_playback->isPlaying()) {
        m_playback->togglePlayback();
    }
    m_playback->stopHitsoundPreview();
    m_playback->setManualPlaytest(false);
    ClipCursor(nullptr);
    RAWINPUTDEVICE mouse{0x01, 0x02, RIDEV_REMOVE, nullptr};
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    m_playtest = false;
    m_viewer->endPlaytest();
    m_viewer->hide();
    m_viewer->setParent(m_viewerEditorParent);
    static_cast<QBoxLayout*>(m_viewerEditorParent->layout())->insertWidget(m_viewerLayoutIndex, m_viewer, 1);
    m_viewer->setFocusPolicy(Qt::NoFocus);
    m_viewer->unsetCursor();
    centralWidget()->show();
    m_viewer->show();
    restoreState(m_playtestWindowState);
    menuBar()->setVisible(m_editorMenuVisible);
    m_toolsToolbar->setVisible(m_editorToolsVisible);
    const std::array<QSplitter*, 4> splitters{m_rootSplitter, m_mainSplitter, m_viewerSplitter, m_bottomSplitter};
    for (int index = 0; index < 4; ++index) {
        splitters[index]->restoreState(m_playtestSplitters[index]);
    }
    if (m_pendingDuration > 0) {
        const qint64 duration = std::exchange(m_pendingDuration, 0);
        updateProjectDuration(duration);
    }
    m_playback->setChartDuration(m_state->chart().durationMilliseconds);
    m_playback->setPosition(m_editorPosition);
    m_state->setPlaybackPosition(m_editorPosition);
    m_viewer->setPlaybackPosition(m_editorPosition);
    if (isActiveWindow()) {
        QCursor::setPos(m_editorCursor);
        if (m_editorFocus) {
            m_editorFocus->setFocus(Qt::OtherFocusReason);
        }
    }
}

void MainWindow::constrainPlaytestCursor() {
    const auto window = reinterpret_cast<HWND>(winId());
    RECT bounds{};
    GetClientRect(window, &bounds);
    POINT topLeft{bounds.left, bounds.top};
    POINT bottomRight{bounds.right, bounds.bottom};
    ClientToScreen(window, &topLeft);
    ClientToScreen(window, &bottomRight);
    bounds = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    ClipCursor(&bounds);
}

MainWindow::PlaytestClock MainWindow::samplePlaytestClock() const {
    const auto clock = m_playback->sampleClock();
    return {m_playtestLeadIn ? std::min(0.0, (clock.monotonicSeconds - m_leadInEnd) * 1000.0)
        : clock.chartMilliseconds, clock.monotonicSeconds};
}

void MainWindow::updatePlaytest() {
    if (!m_playtest || m_playtestFinished) {
        return;
    }
    auto clock = m_playback->sampleClock();
    if (m_playtestLeadIn && clock.monotonicSeconds >= m_leadInEnd) {
        m_playtestLeadIn = false;
        m_playback->togglePlayback();
        clock = m_playback->sampleClock();
    }
    const auto sample = samplePlaytestClock();
    m_playtestSession.advance(sample.chartTime, sample.monotonicTime);
    if (m_playtestSession.complete()) {
        m_playtestFinished = true;
        m_playtestSession.clearInput(clock.monotonicSeconds);
        if (m_playback->isPlaying()) {
            m_playback->togglePlayback();
        }
        m_playtestTimer->stop();
    }
    publishPlaytest();
}

void MainWindow::publishPlaytest() {
    auto judgements = m_playtestSession.takeJudgements();
    for (const auto& judgement : judgements) {
        if (judgement.grade == PlaytestGrade::Miss) {
            continue;
        }
        if (judgement.tickType == 1 || judgement.kind == NoteKind::Flick) {
            HitsoundKind sound = judgement.kind == NoteKind::Flick ? HitsoundKind::FlickHit
                : judgement.kind == NoteKind::Hold ? HitsoundKind::HoldHit
                : judgement.side == FloorSide::Central ? HitsoundKind::NoteHit : HitsoundKind::NoteSideHit;
            m_playback->previewHitsound(m_hitsounds->samples(static_cast<int>(sound)));
        }
    }
    bool hold = false;
    bool sky = false;
    for (const auto& state : m_playtestSession.notes()) {
        hold = hold || (state.active && state.note.kind == NoteKind::Hold);
        sky = sky || (state.active && state.note.kind == NoteKind::Sky);
    }
    if (sky && !m_fieldContact) {
        m_playback->previewHitsound(m_hitsounds->samples(static_cast<int>(HitsoundKind::FieldHit)));
    }
    m_fieldContact = sky;
    m_playback->setHeldHitsounds(hold && !m_playtestFinished, sky && !m_playtestFinished);
    QString status;
    if (m_playtestFinished) {
        status = QStringLiteral("Press Escape to return to editor");
    } else if (m_playtestLeadIn) {
        status = QString::number(std::max(1, static_cast<int>(std::ceil(-m_playtestSession.time() / 1000.0))));
    }
    m_viewer->setPlaytestFrame(m_playtestSession, status, judgements);
}

QString MainWindow::audioPathForFolder(const QString& folderPath) const {
    const QDir folder(folderPath);
    const QFileInfoList audioFiles = folder.entryInfoList(audioNameFilters(), QDir::Files, QDir::Name);

    return audioFiles.isEmpty() ? QString() : audioFiles.front().absoluteFilePath();
}

void MainWindow::startInitialTimingAnalysis() {
    cancelInitialTimingAnalysis();
    m_timingAnalysisInProgress = true;
    m_statusLabel->setText(QStringLiteral("New project created. Detecting initial BPM and offset…"));
}

void MainWindow::analyzeInitialTimingAudio(QByteArray samples, const QAudioFormat& format) {
    if (!m_timingAnalysisInProgress || m_timingWatcher != nullptr) {
        return;
    }
    if (samples.isEmpty() || format.sampleRate() <= 0 || format.channelCount() <= 0
        || format.sampleFormat() != QAudioFormat::Float) {
        cancelInitialTimingAnalysis();
        m_statusLabel->setText(QStringLiteral("New project created; timing detection failed: invalid decoded audio."));
        return;
    }

    QPromise<TimingEstimate> promise;
    m_timingWatcher = new QFutureWatcher<TimingEstimate>(this);
    connect(m_timingWatcher, &QFutureWatcher<TimingEstimate>::finished, this, [this] {
        if (m_timingWatcher->isCanceled()) {
            cancelInitialTimingAnalysis();
            return;
        }
        try {
            const TimingEstimate estimate = m_timingWatcher->result();
            cancelInitialTimingAnalysis();
            finishInitialTimingAnalysis(estimate);
        } catch (const std::exception& error) {
            cancelInitialTimingAnalysis();
            m_statusLabel->setText(QStringLiteral("New project created; timing detection failed: %1")
                .arg(QString::fromUtf8(error.what())));
        }
    });
    m_timingWatcher->setFuture(promise.future());
    QThreadPool::globalInstance()->start(
        [promise = std::move(promise), samples = std::move(samples), format]() mutable {
        promise.start();
        try {
            analyzeDecodedTiming(promise, samples, format);
        } catch (...) {
            promise.setException(std::current_exception());
        }
        promise.finish();
    });
}

void MainWindow::cancelInitialTimingAnalysis() {
    m_timingAnalysisInProgress = false;
    if (m_timingWatcher != nullptr) {
        m_timingWatcher->cancel();
        m_timingWatcher->disconnect(this);
        m_timingWatcher->deleteLater();
        m_timingWatcher = nullptr;
    }
}

void MainWindow::finishInitialTimingAnalysis(const TimingEstimate& estimate) {
    if (!m_hasProject) {
        return;
    }
    if (!estimate.isValid()) {
        m_statusLabel->setText(QStringLiteral("New project created; timing detection found no reliable BPM."));
        return;
    }

    const QVector<TimingPoint> detectedTiming{{
        .timeMilliseconds = estimate.offsetMilliseconds,
        .beatsPerMinute = estimate.beatsPerMinute,
        .timeSignatureNumerator = 4,
        .timeSignatureDenominator = 4,
    }};
    for (DifficultyChart& difficulty : m_project.difficulties) {
        difficulty.timingPoints = detectedTiming;
    }
    m_loadingProject = true;
    m_state->setTimingPoints(detectedTiming);
    m_loadingProject = false;
    m_projectDirty = true;
    m_statusLabel->setText(QStringLiteral("Initial timing detected: %1 BPM, %2 ms offset (%3 interval votes).")
        .arg(estimate.beatsPerMinute, 0, 'f', 0)
        .arg(estimate.offsetMilliseconds)
        .arg(estimate.supportingIntervals));
}

void MainWindow::createProject() {
    if (!confirmDiscardUnsavedChanges(QStringLiteral("creating a new project"))) {
        return;
    }
    QString songPath;
    while (songPath.isEmpty()) {
        const QString selectedPath = QFileDialog::getOpenFileName(this, QStringLiteral("Choose project song"), {},
            audioFileDialogFilter());
        if (selectedPath.isEmpty()) {
            return;
        }
        QString error;
        if (validateAudioSource(selectedPath, &error)) {
            songPath = selectedPath;
        } else {
            QMessageBox::warning(this, QStringLiteral("Choose project song"), error);
        }
    }
    QString jacketPath;
    while (jacketPath.isEmpty()) {
        const QString selectedPath = QFileDialog::getOpenFileName(this, QStringLiteral("Choose jacket"),
            QFileInfo(songPath).absolutePath(), imageFileDialogFilter());
        if (selectedPath.isEmpty()) {
            return;
        }
        QString error;
        if (readJacketImage(selectedPath, nullptr, &error)) {
            jacketPath = selectedPath;
        } else {
            QMessageBox::warning(this, QStringLiteral("Choose jacket"), error);
        }
    }
    const QString songName = QFileInfo(songPath).completeBaseName();
    QByteArray audio;
    QByteArray jacket;
    ChartProject project = blankProject(songName);
    project.songPath = songPath;
    project.jacketPath = jacketPath;
    if (!normalizeAssets(&project, &audio, &jacket, true)) {
        return;
    }
    m_project = std::move(project);
    loadProjectAssets(m_project.songPath, audio, m_project.jacketPath, jacket);
    m_projectPath.clear();
    m_hasProject = true;
    m_loadedDifficulty = Difficulty::Minimal;
    m_loadingProject = true;
    m_state->setMetadata(m_project.metadata);
    m_state->setDifficulty(Difficulty::Minimal);
    m_metadata->setChartId(m_project.chartId);
    m_metadata->setJacketAvailable(true);
    m_metadata->setJacketEditingEnabled(true);
    m_metadata->setDifficultyMetadata(m_project.difficulties.at(0).metadata);
    m_state->setChart(m_project.difficulties.at(0).hitObjects);
    m_state->setTimingPoints(m_project.difficulties.at(0).timingPoints);
    m_state->setLaneEvents(m_project.difficulties.at(0).laneEvents);
    m_state->setSpeedEvents(m_project.difficulties.at(0).speedEvents);
    m_state->setPlaybackPosition(0);
    m_loadingProject = false;
    m_projectDirty = true;
    updateViewerSongCard();
    startInitialTimingAnalysis();
    m_playback->setAudioData(m_projectSongData, inMemoryAudioHint(m_project.songPath));
}

void MainWindow::openProject() {
    if (!confirmDiscardUnsavedChanges(QStringLiteral("opening another project"))) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open project"), m_projectPath,
        QStringLiteral("In Libertas project (*.10no)"));
    if (path.isEmpty()) {
        return;
    }
    loadProject(path);
}

void MainWindow::openRecentProject(const QString& path) {
    if (!confirmDiscardUnsavedChanges(QStringLiteral("opening another project"))) {
        return;
    }
    loadProject(path);
}

void MainWindow::loadProject(const QString& path) {
    const QFileInfo fileInfo(path);
    if (!isProjectDocumentFile(fileInfo)) {
        QMessageBox::warning(this, QStringLiteral("Open project"),
            QStringLiteral("The selected recent project is no longer available."));
        refreshRecentProjectMenu();
        return;
    }

    const QString projectPath = fileInfo.absoluteFilePath();
    ProjectDocument::LoadResult result = ProjectDocument::load(projectPath);
    if (!result.succeeded()) {
        QMessageBox::warning(this, QStringLiteral("Open project"), result.error);
        return;
    }
    QString backupPath;
    if (result.sourceVersion < kProjectVersion) {
        if ((result.sourceVersion < 4 && !normalizeAssets(&result.project, &result.songData, &result.jacketData))
            || !requireChartId(&result.project)) {
            return;
        }
        QString error;
        if (!ProjectDocument::migrate(projectPath, result, &backupPath, &error)) {
            QMessageBox::warning(this, QStringLiteral("Migrate project"), error);
            return;
        }
        result.songFileName = result.project.songPath;
        result.jacketFileName = result.project.jacketPath;
    }
    QImage validatedJacket;
    QString assetError;
    if (!readJacketImage(result.jacketData, &validatedJacket, &assetError)) {
        QMessageBox::warning(this, QStringLiteral("Open project"), assetError);
        return;
    }
    cancelInitialTimingAnalysis();
    m_project = result.project;
    loadProjectAssets(result.songFileName, result.songData, result.jacketFileName, result.jacketData);
    if (m_project.songPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Open project"), QStringLiteral("The project does not contain a song."));
        return;
    }
    const auto populatedDifficulty = std::find_if(m_project.difficulties.cbegin(), m_project.difficulties.cend(),
        [](const DifficultyChart& difficulty) { return !difficulty.hitObjects.notes.isEmpty(); });
    const DifficultyChart& chart = populatedDifficulty == m_project.difficulties.cend()
        ? m_project.difficulties.front() : *populatedDifficulty;

    m_projectPath = projectPath;
    m_hasProject = true;
    m_loadedDifficulty = chart.difficulty;
    m_loadingProject = true;
    m_state->setMetadata(m_project.metadata);
    m_state->setDifficulty(m_loadedDifficulty);
    m_metadata->setChartId(m_project.chartId);
    m_metadata->setJacketAvailable(!m_project.jacketPath.isEmpty());
    m_metadata->setJacketEditingEnabled(true);
    m_metadata->setDifficultyMetadata(chart.metadata);
    m_state->setChart(chart.hitObjects);
    m_state->setTimingPoints(chart.timingPoints);
    m_state->setLaneEvents(chart.laneEvents);
    m_state->setSpeedEvents(chart.speedEvents);
    m_state->setPlaybackPosition(0);
    m_loadingProject = false;
    m_projectDirty = false;
    m_playback->setAudioData(m_projectSongData, inMemoryAudioHint(m_project.songPath));
    updateViewerSongCard();
    m_statusLabel->setText(backupPath.isEmpty() ? QStringLiteral("Opened %1").arg(fileInfo.fileName())
        : QStringLiteral("Migrated project; original backed up to %1").arg(backupPath));
    rememberRecentProject(projectPath);
}

void MainWindow::rememberRecentProject(const QString& path) {
    const QFileInfo fileInfo(path);
    if (!isProjectDocumentFile(fileInfo)) {
        return;
    }

    const QString projectPath = fileInfo.absoluteFilePath();
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    QStringList recentProjects = settings.value(QStringLiteral("recent/projects")).toStringList();
    recentProjects.removeAll(projectPath);
    recentProjects.push_front(projectPath);
    while (recentProjects.size() > kRecentProjectLimit) {
        recentProjects.removeLast();
    }
    settings.setValue(QStringLiteral("recent/projects"), recentProjects);
    refreshRecentProjectMenu();
}

void MainWindow::refreshRecentProjectMenu() {
    if (m_recentProjectMenu == nullptr) {
        return;
    }

    m_recentProjectMenu->clear();
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    const QStringList recentProjects = settings.value(QStringLiteral("recent/projects")).toStringList();
    QStringList existingProjects;
    for (const QString& path : recentProjects) {
        const QFileInfo fileInfo(path);
        const QString projectPath = fileInfo.absoluteFilePath();
        if (!isProjectDocumentFile(fileInfo) || existingProjects.contains(projectPath, Qt::CaseInsensitive)) {
            continue;
        }

        existingProjects.append(projectPath);
        QAction* action = m_recentProjectMenu->addAction(fileInfo.fileName());
        action->setToolTip(projectPath);
        connect(action, &QAction::triggered, this, [this, projectPath] { openRecentProject(projectPath); });
    }
    settings.setValue(QStringLiteral("recent/projects"), existingProjects);
}

bool MainWindow::saveProject() {
    if (!m_hasProject) {
        QMessageBox::information(this, QStringLiteral("Save project"), QStringLiteral("Create or open a project first."));
        return false;
    }
    if (m_projectPath.isEmpty()) {
        saveProjectAs();
        return !m_projectPath.isEmpty() && !m_projectDirty;
    }
    storeLoadedDifficulty();
    if (!requireChartId(&m_project)) {
        return false;
    }
    if (m_projectSongData.isEmpty() || m_projectJacketData.isEmpty()) {
        if (!normalizeAssets(&m_project, &m_projectSongData, &m_projectJacketData)) {
            return false;
        }
        loadProjectAssets(m_project.songPath, m_projectSongData, m_project.jacketPath, m_projectJacketData);
        m_playback->setAudioData(m_projectSongData, inMemoryAudioHint(m_project.songPath));
    }
    m_metadata->setChartId(m_project.chartId);
    QString error;
    const bool saved = ProjectDocument::save(m_projectPath, m_project, m_projectSongData, m_projectJacketData, &error);
    if (!saved) {
        QMessageBox::warning(this, QStringLiteral("Save project"), error);
        return false;
    }
    m_projectDirty = false;
    rememberRecentProject(m_projectPath);
    m_statusLabel->setText(QStringLiteral("Saved %1").arg(QFileInfo(m_projectPath).fileName()));

    return true;
}

void MainWindow::saveProjectAs() {
    if (!m_hasProject) {
        QMessageBox::information(this, QStringLiteral("Save project"), QStringLiteral("Create or open a project first."));
        return;
    }
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save project as"),
        m_projectPath.isEmpty() ? m_project.metadata.songName + QStringLiteral(".10no") : m_projectPath,
        QStringLiteral("In Libertas project (*.10no)"));
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(QStringLiteral(".10no"), Qt::CaseInsensitive)) {
        path.append(QStringLiteral(".10no"));
    }
    const QString previousPath = m_projectPath;
    m_projectPath = path;
    if (!saveProject()) {
        m_projectPath = previousPath;
    }
}

bool MainWindow::requireChartId(ChartProject* project) {
    QString chartId = project->chartId;
    while (!isValidProjectChartId(chartId)) {
        bool accepted = false;
        const QString value = QInputDialog::getText(this, QStringLiteral("Chart ID"),
            QStringLiteral("Enter a Chart ID using letters, digits, '_' or '-'. Reserved filenames are not allowed."),
            QLineEdit::Normal, chartId, &accepted).trimmed();
        if (!accepted) {
            return false;
        }
        chartId = value;
    }
    project->chartId = chartId;

    return true;
}

bool MainWindow::normalizeAssets(ChartProject* project, QByteArray* audio, QByteArray* jacket,
    const bool normalizeAudioPeak) {
    QProgressDialog progress(QStringLiteral("Preparing project media..."), QStringLiteral("Cancel"), 0, 100, this);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    const NormalizedProjectAssets assets = normalizeProjectAssets(project->songPath, *audio, project->jacketPath, *jacket,
        [&progress](const qint64 position, const qint64 duration) {
            progress.setValue(duration > 0 ? static_cast<int>(std::clamp(100 * position / duration, qint64(0), qint64(99))) : 0);
            return !progress.wasCanceled();
        }, normalizeAudioPeak);
    progress.close();
    if (!assets.error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Project media"), assets.error);
        return false;
    }
    *audio = assets.songData;
    *jacket = assets.jacketData;
    project->songPath = QStringLiteral("audio.ogg");
    project->jacketPath = QStringLiteral("jacket.webp");

    return true;
}

void MainWindow::exportProject() {
    if (!m_hasProject) {
        QMessageBox::information(this, QStringLiteral("Export project"), QStringLiteral("Create or open a project first."));
        return;
    }
    storeLoadedDifficulty();
    ChartProject project = m_project;
    QByteArray audio = m_projectSongData;
    QByteArray jacket = m_projectJacketData;
    QString error;
    if (!requireChartId(&project)) {
        return;
    }
    error = projectExportError(project);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Export project"), error);
        return;
    }
    const QStringList exportWarnings = exportWarningsFor(project);
    if (!exportWarnings.isEmpty()) {
        const QString message = QStringLiteral("The project has the following export warnings:\n\n%1\n\nContinue exporting?")
            .arg(exportWarnings.join(QStringLiteral("\n")));
        if (QMessageBox::warning(this, QStringLiteral("Export warnings"), message,
                QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok) {
            return;
        }
    }
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    const QString lastExportDirectory = settings.value(QStringLiteral("export/last_directory")).toString();
    const QString parentPath = QFileDialog::getExistingDirectory(this, QStringLiteral("Export project to"),
        QDir(lastExportDirectory).exists() ? lastExportDirectory : QString());
    if (parentPath.isEmpty()) {
        return;
    }
    const QString outputPath = QDir(parentPath).filePath(project.chartId);
    const QDir outputDirectory(outputPath);
    if (outputDirectory.exists() && !outputDirectory.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty()
        && QMessageBox::question(this, QStringLiteral("Export project"),
            QStringLiteral("%1 already contains files. Replace this export's files?").arg(outputPath),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    if ((audio.isEmpty() || jacket.isEmpty()) && !normalizeAssets(&project, &audio, &jacket)) {
        return;
    }
    if (!writeProjectExport(outputPath, project, audio, jacket, &error)) {
        QMessageBox::warning(this, QStringLiteral("Export project"), error);
        return;
    }
    settings.setValue(QStringLiteral("export/last_directory"), parentPath);
    const bool mediaChanged = audio != m_projectSongData || jacket != m_projectJacketData;
    m_project = std::move(project);
    if (mediaChanged) {
        loadProjectAssets(m_project.songPath, audio, m_project.jacketPath, jacket);
        m_playback->setAudioData(m_projectSongData, inMemoryAudioHint(m_project.songPath));
        updateViewerSongCard();
    }
    m_metadata->setChartId(m_project.chartId);
    markProjectDirty();
    m_statusLabel->setText(QStringLiteral("Exported project to %1").arg(outputPath));
}

void MainWindow::storeLoadedDifficulty() {
    if (!m_hasProject) {
        return;
    }
    DifficultyChart& difficulty = m_project.difficulties.at(difficultyIndex(m_loadedDifficulty));
    difficulty.difficulty = m_loadedDifficulty;
    difficulty.hitObjects = m_state->chart();
    difficulty.timingPoints = m_state->timingPoints();
    difficulty.laneEvents = m_state->laneEvents();
    difficulty.speedEvents = m_state->speedEvents();
    m_project.metadata = m_state->metadata();
}

void MainWindow::loadDifficulty(const Difficulty difficulty) {
    if (!m_hasProject) {
        return;
    }
    const DifficultyChart& chart = m_project.difficulties.at(difficultyIndex(difficulty));
    m_loadingProject = true;
    m_metadata->setDifficultyMetadata(chart.metadata);
    m_state->setChart(chart.hitObjects);
    m_state->setTimingPoints(chart.timingPoints);
    m_state->setLaneEvents(chart.laneEvents);
    m_state->setSpeedEvents(chart.speedEvents);
    m_state->setPlaybackPosition(0);
    m_loadingProject = false;
}

void MainWindow::loadProjectAssets(const QString& songFileName, const QByteArray& songData,
    const QString& jacketFileName, const QByteArray& jacketData) {
    m_projectSongData = songData;
    m_projectJacketData = jacketData;
    m_project.songPath = QFileInfo(songFileName).fileName();
    m_project.jacketPath = QFileInfo(jacketFileName).fileName();
    m_projectJacketImage = {};
    if (!m_projectJacketData.isEmpty()) {
        QString error;
        if (!readJacketImage(m_projectJacketData, &m_projectJacketImage, &error)) {
            m_statusLabel->setText(error);
        }
    }
}

void MainWindow::updateProjectDuration(const qint64 durationMilliseconds) {
    if (m_playtest) {
        m_pendingDuration = durationMilliseconds;
        return;
    }
    if (!m_hasProject || durationMilliseconds <= 0) {
        return;
    }
    bool durationChanged = false;
    for (DifficultyChart& difficulty : m_project.difficulties) {
        durationChanged = durationChanged || difficulty.hitObjects.durationMilliseconds != durationMilliseconds;
        difficulty.hitObjects.durationMilliseconds = durationMilliseconds;
    }
    if (!durationChanged) {
        return;
    }
    m_loadingProject = true;
    ChartData chart = m_state->chart();
    chart.durationMilliseconds = durationMilliseconds;
    m_state->setChart(std::move(chart));
    m_loadingProject = false;
    markProjectDirty();
}

} // namespace infalsus::gui
