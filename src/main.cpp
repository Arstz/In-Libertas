#include "gui/app/main_window.h"
#include "gui/app/dark_theme.h"
#include "gui/app/system_integration.h"

#include <QtCore/QTimer>
#include <QtGui/QSurfaceFormat>
#include <QtWidgets/QApplication>

int main(int argc, char* argv[]) {
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    QSurfaceFormat surfaceFormat = QSurfaceFormat::defaultFormat();
    surfaceFormat.setSamples(4);
    QSurfaceFormat::setDefaultFormat(surfaceFormat);

    QApplication application(argc, argv);
    infalsus::gui::applyDarkTheme(application);
    infalsus::gui::configureSystemIntegration(application);
    const QStringList startupPaths = application.arguments().mid(1);
    infalsus::gui::MainWindow window;
    window.show();
    infalsus::gui::applySystemWindowIcon(window);
    QTimer::singleShot(0, &window, [&window, startupPaths] {
        if (!startupPaths.isEmpty()) {
            window.openProjectFile(startupPaths.front());
        }
        infalsus::gui::offerProjectFileAssociation(window);
    });

    return application.exec();
}
