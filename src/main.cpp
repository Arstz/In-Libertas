#include "gui/app/main_window.h"
#include "gui/app/dark_theme.h"

#include <QtGui/QSurfaceFormat>
#include <QtWidgets/QApplication>

int main(int argc, char* argv[]) {
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    QSurfaceFormat surfaceFormat = QSurfaceFormat::defaultFormat();
    surfaceFormat.setSamples(4);
    QSurfaceFormat::setDefaultFormat(surfaceFormat);

    QApplication application(argc, argv);
    infalsus::gui::applyDarkTheme(application);
    infalsus::gui::MainWindow window;
    window.show();

    return application.exec();
}
