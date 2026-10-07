if(configuration STREQUAL "Debug")
    set(runtime_directory "${vcpkg_directory}/debug/bin")
    set(plugin_directory "${vcpkg_directory}/debug/Qt6/plugins")
    set(debug_suffix "d")
else()
    set(runtime_directory "${vcpkg_directory}/bin")
    set(plugin_directory "${vcpkg_directory}/Qt6/plugins")
    set(debug_suffix "")
endif()

set(plugins
    platforms/qwindows
    multimedia/ffmpegmediaplugin
    imageformats/qjpeg
    imageformats/qgif
    imageformats/qwebp
    imageformats/qico
    imageformats/qtga
)
set(libraries)
foreach(plugin IN LISTS plugins)
    set(source "${plugin_directory}/${plugin}${debug_suffix}.dll")
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Required Qt plugin is missing: ${source}")
    endif()
    get_filename_component(group "${plugin}" DIRECTORY)
    get_filename_component(name "${source}" NAME)
    file(MAKE_DIRECTORY "${output_directory}/${group}")
    file(COPY_FILE "${source}" "${output_directory}/${group}/${name}" ONLY_IF_DIFFERENT)
    list(APPEND libraries "${source}")
endforeach()

foreach(module Core Gui Multimedia Network Widgets OpenGL OpenGLWidgets)
    list(APPEND libraries "${runtime_directory}/Qt6${module}${debug_suffix}.dll")
endforeach()
file(GLOB ffmpeg_libraries
    "${runtime_directory}/avcodec-*.dll"
    "${runtime_directory}/avformat-*.dll"
    "${runtime_directory}/avutil-*.dll"
    "${runtime_directory}/swresample-*.dll"
    "${runtime_directory}/swscale-*.dll"
)
list(APPEND libraries ${ffmpeg_libraries})

set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "dumpbin")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "dumpbin")
file(GET_RUNTIME_DEPENDENCIES
    LIBRARIES ${libraries}
    DIRECTORIES "${runtime_directory}"
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-" "^API-MS-" "^EXT-MS-"
    POST_INCLUDE_REGEXES "^${runtime_directory}/"
    POST_EXCLUDE_REGEXES ".*"
)
if(unresolved)
    message(FATAL_ERROR "Editor runtime dependencies are missing: ${unresolved}")
endif()
foreach(source IN LISTS libraries dependencies)
    get_filename_component(name "${source}" NAME)
    if(NOT source MATCHES "/plugins/")
        file(COPY_FILE "${source}" "${output_directory}/${name}" ONLY_IF_DIFFERENT)
    endif()
endforeach()
