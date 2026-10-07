#include "gui/app/main_window.h"

#include "core/chart_project.h"
#include "core/audio_exporter.h"
#include "core/media_assets.h"
#include "core/timing_analyzer.h"
#include "gui/canvas/flat_view.h"
#include "gui/state/editor_state.h"
#include "gui/widgets/metadata_widget.h"
#include "gui/widgets/properties_panel.h"
#include "gui/widgets/timeline_widget.h"
#include "gui/widgets/timing_widget.h"
#include "gui/widgets/verification_widget.h"
#include "core/chart_document.h"
#include "core/project_document.h"
#include "gui/canvas/conveyor_view.h"
#include "gui/state/playback_controller.h"

#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QSaveFile>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtMultimedia/QAudioBuffer>
#include <QtMultimedia/QAudioDecoder>
#include <QtGui/QActionGroup>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QImage>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QAbstractSpinBox>
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
#include <QtWidgets/QSplitter>
#include <QtWidgets/QSlider>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <cmath>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace infalsus::gui {

namespace {

constexpr int kRecentProjectLimit = 10;

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

[[nodiscard]] bool isTextEntryWidget(const QObject* object) {
    // A spin box gives focus to its internal line edit, but check both the
    // child and its parents so app-level shortcut handling never steals input
    // from a numeric editor.
    for (const QObject* current = object; current != nullptr; current = current->parent()) {
        if (qobject_cast<const QLineEdit*>(current) != nullptr
            || qobject_cast<const QAbstractSpinBox*>(current) != nullptr) {
            return true;
        }
    }

    return false;
}

[[nodiscard]] bool isSkyHitObject(const ChartNote& hitObject) {
    return hitObject.kind == NoteKind::Sky || hitObject.kind == NoteKind::Flick;
}

[[nodiscard]] bool isValidChartId(const QString& chartId) {
    static const QRegularExpression kAllowedCharacters(QStringLiteral("^[A-Za-z0-9_-]+$"));
    return kAllowedCharacters.match(chartId).hasMatch();
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


[[nodiscard]] QJsonValue optionalConfigValue(const QString& value) {
    return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
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
    if (project.metadata.chartDesigner.trimmed().isEmpty()) {
        warnings.append(QStringLiteral("Charter name is empty."));
    }
    if (project.metadata.jacketDesigner.trimmed().isEmpty()) {
        warnings.append(QStringLiteral("Jacket designer name is empty."));
    }
    for (int index = 0; index < 4; ++index) {
        const DifficultyChart& chart = project.difficulties.at(index);
        if (chart.hitObjects.notes.isEmpty()) {
            continue;
        }
        if (chart.metadata.rating > 19) {
            warnings.append(QStringLiteral("%1 rating is higher than 19 and will affect the UI.")
                .arg(difficultyName(difficultyForIndex(index))));
        }
    }
    return warnings;
}

[[nodiscard]] QJsonObject exportConfigFor(const ChartProject& project, const QString& chartId,
    const QString& audioFileName) {
    QJsonArray difficulties;
    for (int index = 0; index < 4; ++index) {
        const DifficultyChart& chart = project.difficulties.at(index);
        if (chart.hitObjects.notes.isEmpty()) {
            // The hook interprets array positions as the native difficulty
            // slots. Keep an explicit null placeholder so a lone Forbidden
            // chart remains slot 3 rather than becoming Minimal.
            difficulties.append(QJsonValue(QJsonValue::Null));
            continue;
        }

        difficulties.append(QJsonObject{
            {QStringLiteral("externalChartId"), chartId + QString::number(index)},
            {QStringLiteral("rating"), chart.metadata.rating},
            {QStringLiteral("levelSectionIndicator"), QString::number(chart.metadata.rating)},
        });
    }
    return {
        {QStringLiteral("enabled"), true},
        {QStringLiteral("templateSongId"), 2},
        {QStringLiteral("chartId"), chartId},
        {QStringLiteral("baseName"), chartId},
        {QStringLiteral("audioFile"), audioFileName},
        {QStringLiteral("jacketLargeFile"), QStringLiteral("jacketLarge.png")},
        {QStringLiteral("jacketSmallFile"), QStringLiteral("jacketSmall.png")},
        {QStringLiteral("enableLoosePngJackets"), true},
        {QStringLiteral("songTitle"), project.metadata.songName},
        {QStringLiteral("artist"), project.metadata.artistName},
        {QStringLiteral("chartDesigner"), project.metadata.chartDesigner},
        {QStringLiteral("jacketDesigner"), project.metadata.jacketDesigner},
        {QStringLiteral("previewStartSeconds"), project.metadata.previewStartSeconds},
        {QStringLiteral("previewEndSeconds"), project.metadata.previewEndSeconds},
        {QStringLiteral("characterIdentifier"), optionalConfigValue(project.metadata.characterIdentifier)},
        {QStringLiteral("gameplayBackground"), optionalConfigValue(project.metadata.gameplayBackground)},
        {QStringLiteral("difficulties"), difficulties},
    };
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

[[nodiscard]] bool hasDefaultInitialTiming(const QVector<TimingPoint>& timingPoints) {
    return timingPoints.size() == 1
        && timingPoints.front().timeMilliseconds == 0
        && std::abs(timingPoints.front().beatsPerMinute - 120.0) < 0.0001
        && timingPoints.front().timeSignatureNumerator == 4
        && timingPoints.front().timeSignatureDenominator == 4;
}

[[nodiscard]] QWidget* createPanel(
    const QString& title,
    QWidget* content,
    QWidget* parent,
    QWidget* titleAccessory = nullptr) {
    auto* panel = new QFrame(parent);
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
    m_playback = new PlaybackController(this);
    m_timingDecoder = new QAudioDecoder(this);
    connect(m_timingDecoder, &QAudioDecoder::bufferReady, this, [this] {
        while (m_timingDecoder->bufferAvailable()) {
            appendInitialTimingAudio(m_timingDecoder->read());
            if (initialTimingAnalysisShouldFinish()) {
                m_timingDecoder->stop();
                QTimer::singleShot(0, this, &MainWindow::finishInitialTimingAnalysis);
                break;
            }
        }
    });
    connect(m_timingDecoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), this,
        [this](const QAudioDecoder::Error) {
        m_timingAnalysisFailed = true;
        m_timingAnalysisError = m_timingDecoder->errorString();
    });
    connect(m_timingDecoder, &QAudioDecoder::finished, this, &MainWindow::finishInitialTimingAnalysis);
    buildInterface();
    buildWorkspace();
    buildToolBar();
    restoreLayout();
    qApp->installEventFilter(this);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
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

    if (event->type() != QEvent::KeyPress) {
        return QMainWindow::eventFilter(watched, event);
    }
    const auto* keyEvent = static_cast<QKeyEvent*>(event);

    // Editor shortcuts are global only while the user is not entering text.
    // In particular, this preserves ordinary letters, Ctrl+C/V, Delete,
    // arrows, and Space for metadata and property line edits.
    if (isTextEntryWidget(QApplication::focusWidget())) {
        return QMainWindow::eventFilter(watched, event);
    }
    // The event list owns Shift/Ctrl for range and toggle selection. Let its
    // view receive those key transitions and its ordinary edit shortcuts.
    if (m_timing != nullptr && (QApplication::focusWidget() == m_timing
        || m_timing->isAncestorOf(QApplication::focusWidget()))
        && (keyEvent->key() == Qt::Key_Shift || keyEvent->key() == Qt::Key_Control)) {
        return QMainWindow::eventFilter(watched, event);
    }

    if ((keyEvent->modifiers() & Qt::ControlModifier) && (keyEvent->modifiers() & Qt::ShiftModifier)
        && keyEvent->key() == Qt::Key_A) {
        m_verification->setChart(m_state->chart(), m_state->speedEvents(), m_state->timingPoints());
        return true;
    }
    if ((keyEvent->modifiers() & Qt::ControlModifier) && keyEvent->key() == Qt::Key_A) {
        selectAllVisibleHitObjects();
        return true;
    }
    if ((keyEvent->modifiers() & Qt::ControlModifier) && keyEvent->key() == Qt::Key_S) {
        saveProject();
        return true;
    }
    if ((keyEvent->modifiers() & Qt::ControlModifier) && keyEvent->key() == Qt::Key_C) {
        m_state->copySelectedHitObjects();
        return true;
    }
    if ((keyEvent->modifiers() & Qt::ControlModifier) && keyEvent->key() == Qt::Key_X) {
        m_state->cutSelectedHitObjects();
        return true;
    }
    if ((keyEvent->modifiers() & Qt::ControlModifier) && keyEvent->key() == Qt::Key_V) {
        m_state->pasteCopiedHitObjects();
        return true;
    }
    if ((keyEvent->modifiers() & Qt::ControlModifier) && keyEvent->key() == Qt::Key_H) {
        m_state->mirrorSelectedHitObjects();
        return true;
    }
    if ((keyEvent->modifiers() & Qt::ControlModifier) && keyEvent->key() == Qt::Key_J) {
        m_state->flipSelectedHitObjectsVertically();
        return true;
    }
    if ((keyEvent->modifiers() & Qt::ControlModifier) && (keyEvent->modifiers() & Qt::ShiftModifier)
        && keyEvent->key() == Qt::Key_E) {
        m_state->resnapAllHitObjects();
        return true;
    }
    switch (keyEvent->key()) {
    case Qt::Key_1:
        m_state->setTool(EditorTool::Place);
        return true;
    case Qt::Key_F:
        m_state->setTool(EditorTool::Select);
        return true;
    case Qt::Key_V:
        m_state->setTool(EditorTool::Move);
        return true;
    case Qt::Key_Delete:
        m_state->removeSelection();
        return true;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        adjustFlatZoom(1.0);
        return true;
    case Qt::Key_Minus:
    case Qt::Key_Underscore:
        adjustFlatZoom(-1.0);
        return true;
    case Qt::Key_Space:
        togglePlayback();
        return true;
    case Qt::Key_BracketLeft:
        m_playback->increasePlaybackRate();
        return true;
    case Qt::Key_BracketRight:
        m_playback->decreasePlaybackRate();
        return true;
    case Qt::Key_Left:
        m_state->seekByDivisor(-1);
        return true;
    case Qt::Key_Right:
        m_state->seekByDivisor(1);
        return true;
    case Qt::Key_Up:
        m_state->increaseDivisor();
        return true;
    case Qt::Key_Down:
        m_state->decreaseDivisor();
        return true;
    case Qt::Key_Tab:
        m_flatModeSelector->setCurrentIndex((m_flatModeSelector->currentIndex() + 1) % m_flatModeSelector->count());
        return true;
    case Qt::Key_Z:
        if (keyEvent->modifiers() & Qt::ControlModifier) {
            m_state->undo();
            return true;
        }
        break;
    case Qt::Key_Y:
        if (keyEvent->modifiers() & Qt::ControlModifier) {
            m_state->redo();
            return true;
        }
        break;
    default:
        return QMainWindow::eventFilter(watched, event);
    }

    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType == "windows_generic_MSG" && message != nullptr) {
        const auto* nativeMessage = static_cast<MSG*>(message);
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
    m_statusLabel = new QLabel(QStringLiteral("Import an SPC file to begin."), centralWidget);
    m_flatView = new FlatView(this);
    m_viewer = new ConveyorView(centralWidget);
    m_timeline = new TimelineWidget(this);
    m_timingPositionLabel = new QLabel(QStringLiteral("00:00:000"), this);

    m_flatModeSelector->addItems({QStringLiteral("Ground"), QStringLiteral("Sky"), QStringLiteral("Both")});
    m_flatModeSelector->setCurrentIndex(2);
    m_noteSpeedSelector->setRange(0.1, 10.0);
    m_noteSpeedSelector->setDecimals(2);
    m_noteSpeedSelector->setSingleStep(0.05);
    m_noteSpeedSelector->setPrefix(QStringLiteral("Conveyor speed "));
    m_noteSpeedSelector->setSuffix(QStringLiteral("x"));
    m_noteSpeedSelector->setToolTip(QStringLiteral("Matches the game's note-speed setting in the 3D view only."));
    QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
    m_noteSpeedSelector->setValue(settings.value(QStringLiteral("viewer/note_speed"), 2.5).toDouble());
    m_viewer->setNoteSpeed(m_noteSpeedSelector->value());
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_contentHost);
    setCentralWidget(centralWidget);
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("File"));
    auto* editMenu = menuBar()->addMenu(QStringLiteral("Edit"));
    auto* windowMenu = menuBar()->addMenu(QStringLiteral("Window"));
    auto* newProjectAction = fileMenu->addAction(QStringLiteral("New project..."));
    auto* openProjectAction = fileMenu->addAction(QStringLiteral("Open project..."));
    m_recentProjectMenu = fileMenu->addMenu(QStringLiteral("Open recent project"));
    auto* importSpcAction = fileMenu->addAction(QStringLiteral("Import SPC..."));
    auto* saveProjectAction = fileMenu->addAction(QStringLiteral("Save project"));
    auto* saveProjectAsAction = fileMenu->addAction(QStringLiteral("Save project as..."));
    auto* exportProjectAction = fileMenu->addAction(QStringLiteral("Export project..."));
    const auto addEditAction = [editMenu](const QString& text, const QKeySequence& shortcut) {
        QAction* action = editMenu->addAction(text);
        action->setShortcut(shortcut);
        // Global key handling deliberately yields to text fields. Keep menu
        // shortcuts visible without allowing QAction to bypass that rule.
        action->setShortcutContext(Qt::WidgetShortcut);
        return action;
    };
    auto* undoAction = addEditAction(QStringLiteral("Undo"), QKeySequence::Undo);
    auto* redoAction = addEditAction(QStringLiteral("Redo"), QKeySequence::Redo);
    editMenu->addSeparator();
    auto* cutAction = addEditAction(QStringLiteral("Cut"), QKeySequence::Cut);
    auto* copyAction = addEditAction(QStringLiteral("Copy"), QKeySequence::Copy);
    auto* pasteAction = addEditAction(QStringLiteral("Paste"), QKeySequence::Paste);
    auto* deleteAction = addEditAction(QStringLiteral("Delete selection"), QKeySequence::Delete);
    editMenu->addSeparator();
    auto* selectAllAction = addEditAction(QStringLiteral("Select all visible"), QKeySequence::SelectAll);
    auto* mirrorAction = addEditAction(QStringLiteral("Mirror selection"), QKeySequence(QStringLiteral("Ctrl+H")));
    auto* verticalFlipAction = addEditAction(QStringLiteral("Flip selection vertically"), QKeySequence(QStringLiteral("Ctrl+J")));
    auto* resnapAllAction = addEditAction(QStringLiteral("Resnap all hit objects"),
        QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    auto* verifyAction = addEditAction(QStringLiteral("Refresh verification"), QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    auto* resetLayoutAction = windowMenu->addAction(QStringLiteral("Reset layout"));
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
    connect(resnapAllAction, &QAction::triggered, m_state, &EditorState::resnapAllHitObjects);
    connect(verifyAction, &QAction::triggered, this, [this] {
        m_verification->setChart(m_state->chart(), m_state->speedEvents(), m_state->timingPoints());
    });
    connect(resetLayoutAction, &QAction::triggered, this, &MainWindow::resetLayout);
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
        QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
        settings.setValue(QStringLiteral("viewer/note_speed"), noteSpeed);
    });
    connect(m_playback, &PlaybackController::positionChanged, m_state, &EditorState::setPlaybackPosition);
    connect(m_playback, &PlaybackController::playbackRateChanged, this, [this](const qreal playbackRate) {
        Q_UNUSED(playbackRate)
        updateStatus();
    });
    connect(m_playback, &PlaybackController::audioError, this, [this](const QString& message) {
        m_statusLabel->setText(QStringLiteral("Audio fallback clock: %1").arg(message));
    });
    connect(m_flatView, &FlatView::waveformError, this, [this](const QString& message) {
        m_statusLabel->setText(QStringLiteral("Waveform decoding failed: %1").arg(message));
    });
    connect(m_playback, &PlaybackController::mediaDurationChanged, this, &MainWindow::updateProjectDuration);
    connect(m_timeline, &TimelineWidget::playbackPositionRequested, m_state, &EditorState::setPlaybackPosition);
    connect(m_flatView, &FlatView::playbackPositionRequested, m_state, &EditorState::setPlaybackPosition);
    connect(m_flatView, &FlatView::divisorSeekRequested, m_state, &EditorState::seekByDivisor);
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
        if (m_state->selectedHitObjects().size() != 1) {
            return;
        }
        QTimer::singleShot(0, m_properties, [this] {
            m_properties->setSelection(*m_state);
        });
    });
    connect(m_state, &EditorState::hitObjectsChanged, this, [this] {
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

void MainWindow::restoreLayout() {
    QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
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
    const bool restoredSplitters = restoredRoot || restoredMain || restoredViewer || restoredBottom;
    m_splittersArranged = restoredSplitters;
}

void MainWindow::saveLayout() const {
    QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
    settings.setValue(QStringLiteral("layout/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("layout/window_state"), saveState());
    settings.setValue(QStringLiteral("layout/root_splitter"), m_rootSplitter->saveState());
    settings.setValue(QStringLiteral("layout/main_splitter"), m_mainSplitter->saveState());
    settings.setValue(QStringLiteral("layout/viewer_splitter"), m_viewerSplitter->saveState());
    settings.setValue(QStringLiteral("layout/bottom_splitter"), m_bottomSplitter->saveState());
}

void MainWindow::resetLayout() {
    QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
    settings.remove(QStringLiteral("layout"));
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
    // This is intentionally not added through QToolBar::addWidget().  That
    // API makes it an action, which puts a tall slider in the toolbar's
    // overflow menu on shorter editor layouts.  A direct child stays exposed
    // below the tool actions and can track the Flat view's playhead.
    m_volumeSlider = new QSlider(Qt::Vertical, m_toolsToolbar);
    m_volumeSlider->setObjectName(QStringLiteral("volumeSlider"));
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setInvertedAppearance(false);
    m_volumeSlider->setToolTip(QStringLiteral("Volume"));
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

    QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
    const int volumePercent = std::clamp(settings.value(QStringLiteral("playback/volume"), 100).toInt(), 0, 100);
    m_volumeSlider->setValue(volumePercent);
    m_playback->setVolume(logarithmicVolumeForPercent(volumePercent));
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
        m_volumeSlider->setToolTip(QStringLiteral("Volume: %1%").arg(volume));
        QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
        settings.setValue(QStringLiteral("playback/volume"), volume);
    });
}

void MainWindow::updateVolumeSliderPlacement() {
    if (m_toolsToolbar == nullptr || m_volumeSlider == nullptr
        || !m_toolsToolbar->isVisible() || !m_flatView->isVisible()) {
        return;
    }

    constexpr int kHorizontalPadding = 7;
    constexpr int kBottomPadding = 8;
    const int desiredTop = m_toolsToolbar->mapFromGlobal(
        m_flatView->mapToGlobal(QPoint(0, m_flatView->playheadViewportY()))).y();
    const int sliderTop = std::clamp(desiredTop, 0, std::max(0, m_toolsToolbar->height() - kBottomPadding));
    const int sliderHeight = std::max(0, m_toolsToolbar->height() - sliderTop - kBottomPadding);
    const int sliderWidth = std::max(0, m_toolsToolbar->width() - kHorizontalPadding * 2);
    m_volumeSlider->setGeometry(kHorizontalPadding, sliderTop, sliderWidth, sliderHeight);
    m_volumeSlider->raise();
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
    auto* flatPanel = createPanel(QStringLiteral("Flat View"), m_flatView, m_mainSplitter, m_flatModeSelector);
    auto* viewerPanel = createPanel(QStringLiteral("3D View"), m_viewer, m_viewerSplitter, m_noteSpeedSelector);
    auto* propertiesPanel = createPanel(QStringLiteral("Properties"), m_properties, m_bottomSplitter);
    auto* metadataPanel = createPanel(QStringLiteral("Metadata"), m_metadata, m_bottomSplitter);
    auto* verificationPanel = createPanel(QStringLiteral("Verification"), m_verification, m_bottomSplitter);
    auto* rightHost = new QWidget(m_rootSplitter);
    auto* rightLayout = new QVBoxLayout(rightHost);
    auto* contentLayout = new QHBoxLayout(m_contentHost);

    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(2);
    rightLayout->addWidget(m_mainSplitter, 1);
    rightLayout->addWidget(m_timeline);
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
        if (!m_projectSongData.isEmpty()) {
            QFile jacketFile(jacketPath);
            if (!jacketFile.open(QIODevice::ReadOnly)) {
                QMessageBox::warning(this, QStringLiteral("Choose jacket"),
                    QStringLiteral("Could not read the selected jacket."));
                return;
            }
            m_projectJacketData = jacketFile.readAll();
            m_projectJacketImage = std::move(jacket);
            m_project.jacketPath = QFileInfo(jacketPath).fileName();
        } else {
            m_project.jacketPath = jacketPath;
        }
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
    connect(m_properties, &PropertiesPanel::hitObjectEditRequested, m_state, &EditorState::editHitObject);
    connect(m_properties, &PropertiesPanel::timingPointEditRequested, m_state, &EditorState::editTimingPoint);
    connect(m_properties, &PropertiesPanel::laneEventEditRequested, m_state, &EditorState::editLaneEvent);
    connect(m_properties, &PropertiesPanel::speedEventEditRequested, m_state, &EditorState::editSpeedEvent);
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
    m_flatView->setAudioSource(audioPath);
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

QString MainWindow::audioPathForFolder(const QString& folderPath) const {
    const QDir folder(folderPath);
    const QFileInfoList audioFiles = folder.entryInfoList(audioNameFilters(), QDir::Files, QDir::Name);

    return audioFiles.isEmpty() ? QString() : audioFiles.front().absoluteFilePath();
}

void MainWindow::startInitialTimingAnalysis(const QString& audioPath) {
    if (m_timingDecoder->isDecoding()) {
        m_timingDecoder->stop();
    }
    m_timingAnalysisSource = audioPath;
    m_timingAnalysisSampleRate = 0;
    m_timingAnalyzer.reset();
    m_timingAnalysisError.clear();
    m_timingAnalysisFailed = !m_timingDecoder->isSupported();
    m_timingAnalysisInProgress = !m_timingAnalysisFailed;
    if (m_timingAnalysisFailed) {
        m_statusLabel->setText(QStringLiteral("New project created; timing analysis is unavailable."));
        return;
    }

    m_statusLabel->setText(QStringLiteral("New project created. Detecting initial BPM and offset…"));
    m_timingDecoder->setSource(QUrl::fromLocalFile(audioPath));
    m_timingDecoder->start();
}

void MainWindow::appendInitialTimingAudio(const QAudioBuffer& buffer) {
    if (!m_timingAnalysisInProgress || !buffer.isValid()) {
        return;
    }

    constexpr qint64 kMaximumAnalysisDurationSeconds = 180;
    const QAudioFormat format = buffer.format();
    const int sampleRate = format.sampleRate();
    const int channels = format.channelCount();
    if (sampleRate <= 0 || channels <= 0) {
        m_timingAnalysisFailed = true;
        m_timingAnalysisError = QStringLiteral("The decoder returned an invalid audio format.");
        return;
    }
    if (m_timingAnalysisSampleRate == 0) {
        m_timingAnalysisSampleRate = sampleRate;
        m_timingAnalyzer = std::make_unique<InitialTimingAnalyzer>(sampleRate);
    } else if (m_timingAnalysisSampleRate != sampleRate) {
        m_timingAnalysisFailed = true;
        m_timingAnalysisError = QStringLiteral("The decoded audio changed sample rates.");
        return;
    }

    const qint64 maximumSamples = static_cast<qint64>(sampleRate) * kMaximumAnalysisDurationSeconds;
    const qint64 remainingSamples = maximumSamples - m_timingAnalyzer->sampleCount();
    if (remainingSamples <= 0) {
        return;
    }
    const qsizetype frameCount = std::min<qint64>(buffer.frameCount(), remainingSamples);
    if (frameCount <= 0) {
        return;
    }

    const auto sampleAt = [&format, &buffer, channels](const qsizetype frame, const int channel) -> float {
        const qsizetype index = frame * channels + channel;
        switch (format.sampleFormat()) {
        case QAudioFormat::UInt8:
            return (static_cast<float>(buffer.constData<quint8>()[index]) - 128.0F) / 128.0F;
        case QAudioFormat::Int16:
            return static_cast<float>(buffer.constData<qint16>()[index]) / 32768.0F;
        case QAudioFormat::Int32:
            return static_cast<float>(buffer.constData<qint32>()[index]) / 2147483648.0F;
        case QAudioFormat::Float:
            return buffer.constData<float>()[index];
        default:
            return 0.0F;
        }
    };

    QVector<float> monoSamples(frameCount);
    for (qsizetype frame = 0; frame < frameCount; ++frame) {
        // TimingAnalyz chooses the strongest channel sample in each analysis
        // window. Retaining the strongest source channel before filtering
        // preserves that emphasis without keeping multichannel PCM in memory.
        float sample = -1.0F;
        for (int channel = 0; channel < channels; ++channel) {
            sample = std::max(sample, sampleAt(frame, channel));
        }
        monoSamples[frame] = std::isfinite(sample) ? sample : 0.0F;
    }
    m_timingAnalyzer->appendSamples(monoSamples.constData(), monoSamples.size());
}

bool MainWindow::initialTimingAnalysisShouldFinish() const {
    if (!m_timingAnalysisInProgress || m_timingAnalysisFailed || m_timingAnalyzer == nullptr
        || m_timingAnalysisSampleRate <= 0) {
        return false;
    }

    constexpr qint64 kMinimumAnalysisDurationSeconds = 60;
    constexpr qint64 kMaximumAnalysisDurationSeconds = 180;
    const qint64 analyzedSamples = m_timingAnalyzer->sampleCount();
    if (analyzedSamples >= static_cast<qint64>(m_timingAnalysisSampleRate) * kMaximumAnalysisDurationSeconds) {
        return true;
    }
    if (analyzedSamples < static_cast<qint64>(m_timingAnalysisSampleRate) * kMinimumAnalysisDurationSeconds) {
        return false;
    }

    const TimingEstimate estimate = m_timingAnalyzer->estimate();
    return estimate.supportingIntervals >= 24 && estimate.confidence() >= 0.15;
}

void MainWindow::finishInitialTimingAnalysis() {
    while (m_timingDecoder->bufferAvailable()) {
        appendInitialTimingAudio(m_timingDecoder->read());
    }
    if (!m_timingAnalysisInProgress) {
        return;
    }
    m_timingAnalysisInProgress = false;
    const QString sourcePath = m_timingAnalysisSource;
    if (m_timingAnalysisFailed) {
        m_statusLabel->setText(QStringLiteral("New project created; timing detection failed: %1")
            .arg(m_timingAnalysisError.isEmpty() ? QStringLiteral("the audio could not be decoded")
                                                    : m_timingAnalysisError));
        return;
    }

    const TimingEstimate estimate = m_timingAnalyzer != nullptr ? m_timingAnalyzer->estimate() : TimingEstimate{};
    m_timingAnalyzer.reset();
    if (!estimate.isValid()) {
        m_statusLabel->setText(QStringLiteral("New project created; timing detection found no reliable BPM."));
        return;
    }
    if (!m_hasProject || m_project.songPath != sourcePath) {
        return;
    }

    const QVector<TimingPoint> detectedTiming{{
        .timeMilliseconds = estimate.offsetMilliseconds,
        .beatsPerMinute = estimate.beatsPerMinute,
        .timeSignatureNumerator = 4,
        .timeSignatureDenominator = 4,
    }};
    bool applied = false;
    for (DifficultyChart& difficulty : m_project.difficulties) {
        if (hasDefaultInitialTiming(difficulty.timingPoints)) {
            difficulty.timingPoints = detectedTiming;
            applied = true;
        }
    }
    if (!applied) {
        return;
    }

    if (hasDefaultInitialTiming(m_state->timingPoints())) {
        m_loadingProject = true;
        m_state->setTimingPoints(detectedTiming);
        m_loadingProject = false;
    }
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
    m_projectSongData.clear();
    m_projectJacketData.clear();
    m_projectJacketImage = {};
    ChartProject project = blankProject(songName);
    project.songPath = songPath;
    project.jacketPath = jacketPath;
    m_project = std::move(project);
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
    m_flatView->setAudioSource(songPath);
    m_playback->setAudioSource(QUrl::fromLocalFile(songPath));
    updateViewerSongCard();
    startInitialTimingAnalysis(songPath);
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
    const ProjectDocument::LoadResult result = ProjectDocument::load(projectPath);
    if (!result.succeeded()) {
        QMessageBox::warning(this, QStringLiteral("Open project"), result.error);
        return;
    }
    m_project = result.project;
    loadProjectAssets(result.songFileName, result.songData, result.jacketFileName, result.jacketData);
    if (m_project.songPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Open project"), QStringLiteral("The project does not contain a song."));
        return;
    }
    m_projectPath = projectPath;
    m_hasProject = true;
    m_loadedDifficulty = Difficulty::Minimal;
    m_loadingProject = true;
    m_state->setMetadata(m_project.metadata);
    m_state->setDifficulty(Difficulty::Minimal);
    m_metadata->setChartId(m_project.chartId);
    m_metadata->setJacketAvailable(!m_project.jacketPath.isEmpty());
    m_metadata->setJacketEditingEnabled(true);
    m_metadata->setDifficultyMetadata(m_project.difficulties.at(0).metadata);
    m_state->setChart(m_project.difficulties.at(0).hitObjects);
    m_state->setTimingPoints(m_project.difficulties.at(0).timingPoints);
    m_state->setLaneEvents(m_project.difficulties.at(0).laneEvents);
    m_state->setSpeedEvents(m_project.difficulties.at(0).speedEvents);
    m_state->setPlaybackPosition(0);
    m_loadingProject = false;
    m_projectDirty = false;
    m_flatView->setAudioData(m_projectSongData, m_project.songPath);
    m_playback->setAudioData(m_projectSongData, inMemoryAudioHint(m_project.songPath));
    updateViewerSongCard();
    m_statusLabel->setText(QStringLiteral("Opened %1").arg(fileInfo.fileName()));
    rememberRecentProject(projectPath);
}

void MainWindow::rememberRecentProject(const QString& path) {
    const QFileInfo fileInfo(path);
    if (!isProjectDocumentFile(fileInfo)) {
        return;
    }

    const QString projectPath = fileInfo.absoluteFilePath();
    QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
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
    QSettings settings(QStringLiteral("InFalsusDump"), QStringLiteral("In Libertas"));
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

void MainWindow::saveProject() {
    if (!m_hasProject) {
        QMessageBox::information(this, QStringLiteral("Save project"), QStringLiteral("Create or open a project first."));
        return;
    }
    if (m_projectPath.isEmpty()) {
        saveProjectAs();
        return;
    }
    storeLoadedDifficulty();
    QString error;
    const bool saved = m_projectSongData.isEmpty()
        ? ProjectDocument::save(m_projectPath, m_project, &error)
        : ProjectDocument::save(m_projectPath, m_project, m_projectSongData, m_projectJacketData, &error);
    if (!saved) {
        QMessageBox::warning(this, QStringLiteral("Save project"), error);
        return;
    }
    m_projectDirty = false;
    rememberRecentProject(m_projectPath);
    m_statusLabel->setText(QStringLiteral("Saved %1").arg(QFileInfo(m_projectPath).fileName()));
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
    m_projectPath = path;
    saveProject();
}

void MainWindow::exportProject() {
    if (!m_hasProject) {
        QMessageBox::information(this, QStringLiteral("Export project"), QStringLiteral("Create or open a project first."));
        return;
    }
    storeLoadedDifficulty();
    if (m_project.songPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Export project"), QStringLiteral("The project has no song."));
        return;
    }
    QImage jacket;
    QString error;
    if (!readProjectJacket(&jacket, &error)) {
        QMessageBox::warning(this, QStringLiteral("Export project"), error);
        return;
    }
    bool hasHitObjects = false;
    for (const DifficultyChart& chart : m_project.difficulties) {
        if (!chart.hitObjects.notes.isEmpty()) {
            hasHitObjects = true;
            break;
        }
    }
    if (!hasHitObjects) {
        QMessageBox::warning(this, QStringLiteral("Export project"),
            QStringLiteral("Add at least one game object to a difficulty before exporting."));
        return;
    }
    if (m_project.metadata.previewStartSeconds >= m_project.metadata.previewEndSeconds) {
        QMessageBox::warning(this, QStringLiteral("Export project"),
            QStringLiteral("Preview start time must be earlier than preview end time."));
        return;
    }
    const QStringList exportWarnings = exportWarningsFor(m_project);
    if (!exportWarnings.isEmpty()) {
        const QString message = QStringLiteral("The project has the following export warnings:\n\n%1\n\nContinue exporting?")
            .arg(exportWarnings.join(QStringLiteral("\n")));
        if (QMessageBox::warning(this, QStringLiteral("Export warnings"), message,
                QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok) {
            return;
        }
    }

    QString chartId = m_project.chartId.trimmed();
    if (chartId.isEmpty()) {
        bool accepted = false;
        chartId = QInputDialog::getText(this, QStringLiteral("Chart ID"),
            QStringLiteral("Chart ID"), QLineEdit::Normal, {}, &accepted).trimmed();
        if (!accepted) {
            return;
        }
    }
    if (!isValidChartId(chartId)) {
        QMessageBox::warning(this, QStringLiteral("Export project"),
            QStringLiteral("Chart ID must contain only letters, digits, '_' or '-'."));
        return;
    }
    const QString parentPath = QFileDialog::getExistingDirectory(this, QStringLiteral("Export project to"));
    if (parentPath.isEmpty()) {
        return;
    }
    const QString outputPath = QDir(parentPath).filePath(chartId);
    const QDir outputDirectory(outputPath);
    if (outputDirectory.exists() && !outputDirectory.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty()
        && QMessageBox::question(this, QStringLiteral("Export project"),
            QStringLiteral("%1 already contains files. Replace this export's files?").arg(outputPath),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    if (!QDir().mkpath(outputPath)) {
        QMessageBox::warning(this, QStringLiteral("Export project"), QStringLiteral("Could not create the export folder."));
        return;
    }

    const QString audioFileName = QStringLiteral("audio.ogg");
    const QString audioOutputPath = outputDirectory.filePath(audioFileName);
    QProgressDialog progress(QStringLiteral("Exporting Ogg Vorbis audio..."), QStringLiteral("Cancel"), 0, 100, this);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    const bool audioExported = exportOggAudio(m_project.songPath, m_projectSongData, audioOutputPath, &error,
        [&progress](qint64 position, qint64 duration) {
        progress.setValue(duration > 0 ? static_cast<int>(std::clamp(100 * position / duration, qint64(0), qint64(99))) : 0);
        return !progress.wasCanceled();
    });
    progress.close();
    if (!audioExported
        || !writeJacketImage(jacket, outputDirectory.filePath(QStringLiteral("jacketLarge.png")), &error)
        || !writeJacketImage(jacket, outputDirectory.filePath(QStringLiteral("jacketSmall.png")), &error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("Could not write the jacket PNG files.");
        }
        QMessageBox::warning(this, QStringLiteral("Export project"), error);
        return;
    }

    for (int index = 0; index < 4; ++index) {
        DifficultyChart& chart = m_project.difficulties.at(index);
        if (chart.hitObjects.notes.isEmpty()) {
            continue;
        }
        const QString chartStem = chartId + QString::number(index);
        if (!ChartDocument::save(outputDirectory.filePath(chartStem + QStringLiteral(".spc")),
                chart.hitObjects, chart.timingPoints, chart.laneEvents, chart.speedEvents, &error)) {
            QMessageBox::warning(this, QStringLiteral("Export project"), error);
            return;
        }
    }
    const QByteArray configData = QJsonDocument(exportConfigFor(m_project, chartId, audioFileName)).toJson(QJsonDocument::Indented);
    QSaveFile configFile(outputDirectory.filePath(QStringLiteral("config.json")));
    if (!configFile.open(QIODevice::WriteOnly)
        || configFile.write(configData) != configData.size()
        || !configFile.commit()) {
        QMessageBox::warning(this, QStringLiteral("Export project"), QStringLiteral("Could not write config.json."));
        return;
    }
    m_project.chartId = chartId;
    m_metadata->setChartId(chartId);
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
