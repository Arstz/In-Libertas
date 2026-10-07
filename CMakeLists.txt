cmake_minimum_required(VERSION 3.24)

project(InLibertas LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Multimedia OpenGL OpenGLWidgets Widgets)
find_package(FFmpeg REQUIRED COMPONENTS AVCODEC AVFORMAT)

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
    src/core/audio_exporter.cpp
    src/core/audio_exporter.h
    src/core/media_assets.cpp
    src/core/media_assets.h
    src/gui/app/main_window.cpp
    src/gui/app/main_window.h
    src/gui/app/settings_dialog.cpp
    src/gui/app/settings_dialog.h
    src/gui/app/visual_settings.h
    src/gui/app/handling_settings.h
    src/gui/key_bindings.cpp
    src/gui/key_bindings.h
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

target_include_directories(InLibertas PRIVATE src ${FFMPEG_INCLUDE_DIRS})

target_link_libraries(InLibertas PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Multimedia
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Widgets
    FFmpeg::avcodec
    FFmpeg::avformat
)

add_custom_command(TARGET InLibertas POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        "-Dvcpkg_directory=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
        "-Doutput_directory=$<TARGET_FILE_DIR:InLibertas>"
        "-Dconfiguration=$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/deploy_editor_runtime.cmake"
    VERBATIM
)

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(media_assets_test
        tests/core/media_assets_test.cpp
        src/core/media_assets.cpp
        src/core/audio_exporter.cpp
    )
    target_include_directories(media_assets_test PRIVATE src ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(media_assets_test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Multimedia
        FFmpeg::avcodec FFmpeg::avformat
    )
    add_dependencies(media_assets_test InLibertas)
    add_test(NAME media_assets COMMAND media_assets_test)
endif()
