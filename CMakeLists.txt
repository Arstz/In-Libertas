cmake_minimum_required(VERSION 3.24)

project(InLibertas LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Multimedia Network OpenGL OpenGLWidgets Widgets)
find_package(FFmpeg REQUIRED COMPONENTS AVCODEC AVFORMAT AVUTIL)
find_package(WebP CONFIG REQUIRED)
find_path(SIGNALSMITH_STRETCH_INCLUDE_DIR signalsmith-stretch/signalsmith-stretch.h REQUIRED)

add_library(InLibertasJacketCodec SHARED src/core/jacket_codec.cpp)
set_target_properties(InLibertasJacketCodec PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_definitions(InLibertasJacketCodec PRIVATE INLIBERTAS_JACKET_CODEC_BUILD)
target_link_libraries(InLibertasJacketCodec PRIVATE WebP::webp)
add_custom_command(TARGET InLibertasJacketCodec POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Mods"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:InLibertasJacketCodec>" "$<TARGET_FILE:WebP::webp>" "$<TARGET_FILE:WebP::sharpyuv>"
        "${CMAKE_BINARY_DIR}/Mods"
    VERBATIM
)

qt_add_executable(InLibertas WIN32
    src/main.cpp
    src/core/chart_project.cpp
    src/core/chart_project.h
    src/core/timing_analyzer.cpp
    src/core/timing_analyzer.h
    src/core/timing_grid.h
    src/core/project_document.cpp
    src/core/project_format.cpp
    src/core/project_assets.cpp
    src/core/project_export.cpp
    src/core/project_document.h
    src/core/chart_document.cpp
    src/core/chart_document.h
    src/core/chart_types.h
    src/core/playtest_session.cpp
    src/core/playtest_session.h
    src/core/field_groups.cpp
    src/core/field_groups.h
    src/core/field_presets.cpp
    src/core/field_presets.h
    src/core/audio_exporter.cpp
    src/core/audio_exporter.h
    src/core/media_assets.cpp
    src/core/media_assets.h
    src/core/waveform.cpp
    src/core/waveform.h
    src/gui/app/main_window.cpp
    src/gui/app/main_window.h
    src/gui/app/settings_dialog.cpp
    src/gui/app/settings_dialog.h
    src/gui/app/system_integration.cpp
    src/gui/app/system_integration.h
    src/gui/app/chartbox_dialog.cpp
    src/gui/app/chartbox_dialog.h
    src/gui/app/sv_interpolation_dialog.cpp
    src/gui/app/sv_interpolation_dialog.h
    src/gui/app/visual_settings.h
    src/gui/app/playtest_settings.h
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
    src/gui/widgets/playback_controls.cpp
    src/gui/widgets/playback_controls.h
    src/gui/widgets/timing_widget.cpp
    src/gui/widgets/timing_widget.h
    src/gui/widgets/field_presets_widget.cpp
    src/gui/widgets/field_presets_widget.h
    src/gui/widgets/verification_widget.cpp
    src/gui/widgets/verification_widget.h
    src/gui/state/playback_controller.cpp
    src/gui/state/playback_controller.h
    src/gui/state/audio_playback_stream.cpp
    src/gui/state/audio_playback_stream.h
    src/gui/state/hitsound_bank.cpp
    src/gui/state/hitsound_bank.h
    src/gui/state/playback_rates.h
    src/gui/canvas/conveyor_view.cpp
    src/gui/canvas/conveyor_view.h
    src/gui/canvas/conveyor_geometry.cpp
    src/gui/canvas/conveyor_geometry.h
)

set_target_properties(InLibertas PROPERTIES OUTPUT_NAME "In Libertas")

qt_add_resources(InLibertas system_icons
    PREFIX "/system"
    BASE "assets/system"
    FILES "assets/system/Logo.png"
)

if(WIN32)
    enable_language(RC)
    find_program(POWERSHELL_EXECUTABLE NAMES powershell
        HINTS "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0"
        NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    set(INLIBERTAS_APPLICATION_ICON "${CMAKE_CURRENT_BINARY_DIR}/system-icons/application.ico")
    set(INLIBERTAS_PROJECT_ICON "${CMAKE_CURRENT_BINARY_DIR}/system-icons/project.ico")
    add_custom_command(
        OUTPUT "${INLIBERTAS_APPLICATION_ICON}" "${INLIBERTAS_PROJECT_ICON}"
        COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_system_icons.ps1"
            -SourceDirectory "${CMAKE_CURRENT_SOURCE_DIR}/assets/system"
            -OutputDirectory "${CMAKE_CURRENT_BINARY_DIR}/system-icons"
        DEPENDS tools/generate_system_icons.ps1
            "assets/system/Logo.png" "assets/system/File small.png" "assets/system/File big.png"
        VERBATIM
    )
    add_custom_target(InLibertasSystemIcons
        DEPENDS "${INLIBERTAS_APPLICATION_ICON}" "${INLIBERTAS_PROJECT_ICON}")
    add_dependencies(InLibertas InLibertasSystemIcons)
    set(INLIBERTAS_WINDOWS_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/windows_resources.rc")
    configure_file(src/gui/app/windows_resources.rc.in "${INLIBERTAS_WINDOWS_RESOURCES}" @ONLY)
    set_property(SOURCE "${INLIBERTAS_WINDOWS_RESOURCES}" APPEND PROPERTY OBJECT_DEPENDS
        "${INLIBERTAS_APPLICATION_ICON}" "${INLIBERTAS_PROJECT_ICON}")
    target_sources(InLibertas PRIVATE "${INLIBERTAS_WINDOWS_RESOURCES}")
    target_link_libraries(InLibertas PRIVATE shell32 user32)
endif()

add_custom_target(InLibertasAssets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:InLibertas>/assets"
    VERBATIM
)
add_dependencies(InLibertas InLibertasAssets)

target_include_directories(InLibertas PRIVATE src ${FFMPEG_INCLUDE_DIRS} ${SIGNALSMITH_STRETCH_INCLUDE_DIR})

target_link_libraries(InLibertas PRIVATE
    InLibertasJacketCodec
    Qt6::Core
    Qt6::Gui
    Qt6::Multimedia
    Qt6::Network
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Widgets
    FFmpeg::avcodec
    FFmpeg::avformat
    FFmpeg::avutil
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
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/gui/event_performance_test.cpp")
        qt_add_executable(event_performance_test
            tests/gui/event_performance_test.cpp
            src/gui/widgets/timing_widget.h
            src/gui/widgets/timeline_widget.h
        )
        target_include_directories(event_performance_test PRIVATE src)
        target_link_libraries(event_performance_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
        add_dependencies(event_performance_test InLibertas)
        add_test(NAME event_performance COMMAND event_performance_test -platform offscreen)
        set_tests_properties(event_performance PROPERTIES ENVIRONMENT
            "QT_QPA_PLATFORM_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<$<CONFIG:Debug>:debug/>Qt6/plugins/platforms")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/core/field_presets_test.cpp")
        qt_add_executable(field_presets_test tests/core/field_presets_test.cpp
            src/core/field_presets.cpp src/core/chart_document.cpp src/core/chart_project.cpp)
        target_include_directories(field_presets_test PRIVATE src)
        target_link_libraries(field_presets_test PRIVATE Qt6::Core)
        add_test(NAME field_presets COMMAND field_presets_test)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/repair_field_segments.cpp")
        qt_add_executable(repair_field_segments tests/repair_field_segments.cpp
            src/core/chart_document.cpp src/core/chart_project.cpp)
        target_include_directories(repair_field_segments PRIVATE src)
        target_link_libraries(repair_field_segments PRIVATE Qt6::Core)
    endif()
    if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hook/native_runtime_overrides_test.cpp")
        add_executable(native_runtime_overrides_test tests/hook/native_runtime_overrides_test.cpp)
        set_target_properties(native_runtime_overrides_test PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
        add_test(NAME native_runtime_overrides COMMAND native_runtime_overrides_test)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/gui/playtest_settings_test.cpp")
        qt_add_executable(playtest_settings_test
            tests/gui/playtest_settings_test.cpp
            src/core/chart_project.cpp
            src/gui/widgets/metadata_widget.cpp
            src/gui/widgets/metadata_widget.h
            src/gui/key_bindings.cpp
            src/gui/state/audio_playback_stream.cpp
            src/gui/state/playback_controller.cpp
            src/gui/state/playback_controller.h
            src/gui/state/hitsound_bank.cpp
            src/gui/state/hitsound_bank.h
            src/core/waveform.cpp
            src/core/media_assets.cpp
            src/gui/app/system_integration.cpp
        )
        target_include_directories(playtest_settings_test PRIVATE src ${SIGNALSMITH_STRETCH_INCLUDE_DIR} ${FFMPEG_INCLUDE_DIRS})
        target_link_libraries(playtest_settings_test PRIVATE InLibertasJacketCodec Qt6::Core Qt6::Gui Qt6::Widgets
            Qt6::Multimedia FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil)
        if(WIN32)
            target_link_libraries(playtest_settings_test PRIVATE shell32 user32)
        endif()
        add_dependencies(playtest_settings_test InLibertas)
        add_test(NAME playtest_settings COMMAND playtest_settings_test -platform offscreen)
        set_tests_properties(playtest_settings PROPERTIES ENVIRONMENT
            "QT_QPA_PLATFORM_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<$<CONFIG:Debug>:debug/>Qt6/plugins/platforms")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/gui/conveyor_performance_test.cpp")
        qt_add_executable(conveyor_performance_test
            tests/gui/conveyor_performance_test.cpp
            src/gui/canvas/conveyor_view.h
            src/gui/canvas/conveyor_geometry.cpp
            src/core/playtest_session.cpp
            src/core/chart_document.cpp
            src/core/field_groups.cpp
        )
        target_include_directories(conveyor_performance_test PRIVATE src)
        target_link_libraries(conveyor_performance_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGLWidgets)
        add_dependencies(conveyor_performance_test InLibertas)
        add_test(NAME conveyor_performance COMMAND conveyor_performance_test -platform offscreen)
        set_tests_properties(conveyor_performance PROPERTIES ENVIRONMENT
            "QT_QPA_PLATFORM_PLUGIN_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<$<CONFIG:Debug>:debug/>Qt6/plugins/platforms")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/core/playtest_session_test.cpp")
        qt_add_executable(playtest_session_test
            tests/core/playtest_session_test.cpp
            src/core/playtest_session.cpp
            src/core/chart_document.cpp
        )
        target_include_directories(playtest_session_test PRIVATE src)
        target_link_libraries(playtest_session_test PRIVATE Qt6::Core)
        add_dependencies(playtest_session_test InLibertas)
        add_test(NAME playtest_session COMMAND playtest_session_test)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/gui/audio_playback_stream_test.cpp")
        qt_add_executable(audio_playback_stream_test
            tests/gui/audio_playback_stream_test.cpp
            src/gui/state/audio_playback_stream.cpp
            src/gui/state/playback_controller.cpp
            src/gui/state/playback_controller.h
            src/gui/state/hitsound_bank.cpp
            src/gui/state/hitsound_bank.h
            src/core/waveform.cpp
        )
        target_include_directories(audio_playback_stream_test PRIVATE src ${SIGNALSMITH_STRETCH_INCLUDE_DIR})
        target_link_libraries(audio_playback_stream_test PRIVATE Qt6::Core Qt6::Multimedia)
        add_dependencies(audio_playback_stream_test InLibertas)
        add_test(NAME audio_playback_stream COMMAND audio_playback_stream_test)
    endif()
    qt_add_executable(media_assets_test
        tests/core/media_assets_test.cpp
        src/core/media_assets.cpp
        src/core/audio_exporter.cpp
        src/core/project_format.cpp
    )
    target_include_directories(media_assets_test PRIVATE src ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(media_assets_test PRIVATE
        InLibertasJacketCodec
        Qt6::Core Qt6::Gui Qt6::Multimedia
        FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil
    )
    add_dependencies(media_assets_test InLibertas)
    add_test(NAME media_assets COMMAND media_assets_test)
    qt_add_executable(project_pipeline_test
        tests/core/project_pipeline_test.cpp
        src/core/project_assets.cpp
        src/core/project_format.cpp
        src/core/project_document.cpp
        src/core/project_export.cpp
        src/core/chart_project.cpp
        src/core/chart_document.cpp
        src/core/audio_exporter.cpp
        src/core/media_assets.cpp
    )
    target_include_directories(project_pipeline_test PRIVATE src ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(project_pipeline_test PRIVATE InLibertasJacketCodec Qt6::Core Qt6::Gui Qt6::Multimedia FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil)
    add_dependencies(project_pipeline_test InLibertas)
    add_test(NAME project_pipeline COMMAND project_pipeline_test "${CMAKE_BINARY_DIR}/pipeline-fixtures")
    set_tests_properties(project_pipeline PROPERTIES FIXTURES_SETUP project_assets)
    find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
    add_test(NAME hook_project_pipeline COMMAND "${DOTNET_EXECUTABLE}" run
        --project "${CMAKE_CURRENT_SOURCE_DIR}/tests/hook/project_pipeline_test.csproj"
        --configuration Release -- "${CMAKE_BINARY_DIR}/pipeline-fixtures")
    set_tests_properties(hook_project_pipeline PROPERTIES FIXTURES_REQUIRED project_assets)
    qt_add_executable(field_groups_test
        tests/core/field_groups_test.cpp
        src/core/field_groups.cpp
        src/core/field_presets.cpp
        src/core/chart_document.cpp
        src/core/chart_project.cpp
        src/core/project_document.cpp
        src/core/project_format.cpp
        src/gui/state/editor_state.cpp
        src/gui/state/editor_state.h
    )
    target_include_directories(field_groups_test PRIVATE src)
    target_link_libraries(field_groups_test PRIVATE InLibertasJacketCodec Qt6::Core)
    add_dependencies(field_groups_test InLibertas)
    add_test(NAME field_groups COMMAND field_groups_test)
    set_tests_properties(field_groups PROPERTIES FIXTURES_REQUIRED project_assets)
    qt_add_executable(batch_properties_test
        tests/core/batch_properties_test.cpp
        src/core/chart_project.cpp
        src/core/field_groups.cpp
        src/gui/state/editor_state.cpp
        src/gui/state/editor_state.h
    )
    target_include_directories(batch_properties_test PRIVATE src)
    target_link_libraries(batch_properties_test PRIVATE Qt6::Core)
    add_dependencies(batch_properties_test InLibertas)
    add_test(NAME batch_properties COMMAND batch_properties_test)
endif()
