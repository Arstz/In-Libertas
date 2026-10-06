cmake_minimum_required(VERSION 3.24)

project(InLibertas LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Multimedia OpenGL OpenGLWidgets Widgets)

qt_add_executable(InLibertas WIN32
    src/main.cpp
    src/core/chart_project.cpp
    src/core/chart_project.h
    src/core/timing_analyzer.cpp
    src/core/timing_analyzer.h
    src/core/timing_grid.h
    src/core/project_document.cpp
    src/core/project_document.h
    src/core/chart_document.cpp
    src/core/chart_document.h
    src/core/chart_types.h
    src/gui/app/main_window.cpp
    src/gui/app/main_window.h
    src/gui/app/dark_theme.cpp
    src/gui/app/dark_theme.h
    src/gui/app/game_palette.h
    src/gui/canvas/flat_view.cpp
    src/gui/canvas/flat_view.h
    src/gui/state/editor_state.cpp
    src/gui/state/editor_state.h
    src/gui/widgets/metadata_widget.cpp
    src/gui/widgets/metadata_widget.h
    src/gui/widgets/properties_panel.cpp
    src/gui/widgets/properties_panel.h
    src/gui/widgets/timeline_widget.cpp
    src/gui/widgets/timeline_widget.h
    src/gui/widgets/timing_widget.cpp
    src/gui/widgets/timing_widget.h
    src/gui/widgets/verification_widget.cpp
    src/gui/widgets/verification_widget.h
    src/gui/state/playback_controller.cpp
    src/gui/state/playback_controller.h
    src/gui/canvas/conveyor_view.cpp
    src/gui/canvas/conveyor_view.h
    src/gui/canvas/conveyor_geometry.cpp
    src/gui/canvas/conveyor_geometry.h
)

set_target_properties(InLibertas PROPERTIES OUTPUT_NAME "In Libertas")

target_include_directories(InLibertas PRIVATE src)

target_link_libraries(InLibertas PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Multimedia
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Widgets
)

# Qt's platform plugins are not linked into the executable. Keep the native
# Windows backend beside the Release binary so it also launches from Explorer.
add_custom_command(TARGET InLibertas POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InLibertas>/platforms"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:InLibertas>/multimedia"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins/platforms/qwindows.dll"
        "$<TARGET_FILE_DIR:InLibertas>/platforms/qwindows.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins/multimedia/ffmpegmediaplugin.dll"
        "$<TARGET_FILE_DIR:InLibertas>/multimedia/ffmpegmediaplugin.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins/multimedia/windowsmediaplugin.dll"
        "$<TARGET_FILE_DIR:InLibertas>/multimedia/windowsmediaplugin.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/avformat-62.dll"
        "$<TARGET_FILE_DIR:InLibertas>/avformat-62.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/avcodec-62.dll"
        "$<TARGET_FILE_DIR:InLibertas>/avcodec-62.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/swresample-6.dll"
        "$<TARGET_FILE_DIR:InLibertas>/swresample-6.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/swscale-9.dll"
        "$<TARGET_FILE_DIR:InLibertas>/swscale-9.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/avutil-60.dll"
        "$<TARGET_FILE_DIR:InLibertas>/avutil-60.dll"
)
