cmake_minimum_required(VERSION 3.20)

project(AveMediaBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(AVEMEDIABRIDGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
get_filename_component(FFMPEG_ROOT "${AVEMEDIABRIDGE_ROOT}/ffmpeg" ABSOLUTE)
get_filename_component(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include" ABSOLUTE)
get_filename_component(FFMPEG_LIB_DIR "${FFMPEG_ROOT}/lib" ABSOLUTE)
get_filename_component(FFMPEG_BIN_DIR "${FFMPEG_ROOT}/bin" ABSOLUTE)

file(GLOB FFMPEG_RUNTIME_DLLS CONFIGURE_DEPENDS
    "${FFMPEG_BIN_DIR}/avformat-*.dll"
    "${FFMPEG_BIN_DIR}/avcodec-*.dll"
    "${FFMPEG_BIN_DIR}/avutil-*.dll"
    "${FFMPEG_BIN_DIR}/swresample-*.dll"
)

function(copy_ffmpeg_runtime_dlls target_name)
    foreach(dll_file IN LISTS FFMPEG_RUNTIME_DLLS)
        add_custom_command(TARGET "${target_name}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:${target_name}>/Lib/ffmpeg"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${dll_file}"
                "$<TARGET_FILE_DIR:${target_name}>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${dll_file}"
                "$<TARGET_FILE_DIR:${target_name}>/Lib/ffmpeg"
            VERBATIM
        )
    endforeach()
endfunction()

add_library(AveMediaBridgeCore STATIC
    src/AveMediaBridge.cpp
    src/Core/AudioBufferF32.cpp
    src/Core/AudioStats.cpp
    src/Core/MediaBridgeError.cpp
    src/Export/FfmpegWavAudioExporter.cpp
    src/Import/FfmpegAudioImporter.cpp
    src/Process/AudioBufferProcessor.cpp
)

target_include_directories(AveMediaBridgeCore
    PUBLIC
        "${AVEMEDIABRIDGE_ROOT}/include"
    PRIVATE
        "${FFMPEG_INCLUDE_DIR}"
)

target_link_libraries(AveMediaBridgeCore
    PUBLIC
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swresample.lib"
)

add_library(AveMediaBridge SHARED
    src/Dll/AveMediaBridgeDll.cpp
    src/Ffmpeg/FfmpegStreamSelection.cpp
    src/Probe/FrameCountPolicy.cpp
    src/Probe/MediaProbeService.cpp
    src/Probe/PacketScan.cpp
    src/Probe/ProbeJsonWriter.cpp
    src/Utils/JsonUtils.cpp
)

target_link_libraries(AveMediaBridge PRIVATE AveMediaBridgeCore)
target_include_directories(AveMediaBridge PRIVATE "${FFMPEG_INCLUDE_DIR}")
target_compile_definitions(AveMediaBridge PRIVATE AVEMEDIABRIDGE_BUILD_DLL)
set_target_properties(AveMediaBridge PROPERTIES OUTPUT_NAME "AveMediaBridge")

add_executable(AveMediaBridgeLabApp
    app/main.cpp
)

target_link_libraries(AveMediaBridgeLabApp PRIVATE AveMediaBridgeCore)
target_include_directories(AveMediaBridgeLabApp PRIVATE "${FFMPEG_INCLUDE_DIR}")
target_compile_definitions(AveMediaBridgeLabApp PRIVATE
    AVEMEDIABRIDGE_PROJECT_ROOT="${AVEMEDIABRIDGE_ROOT}"
    AVEMEDIABRIDGE_FFMPEG_INCLUDE="${FFMPEG_INCLUDE_DIR}"
    AVEMEDIABRIDGE_FFMPEG_LIB="${FFMPEG_LIB_DIR}"
    AVEMEDIABRIDGE_FFMPEG_BIN="${FFMPEG_BIN_DIR}"
)

add_dependencies(AveMediaBridgeLabApp AveMediaBridge)

add_custom_command(TARGET AveMediaBridgeLabApp POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:AveMediaBridge>"
        "$<TARGET_FILE_DIR:AveMediaBridgeLabApp>"
    VERBATIM
)

copy_ffmpeg_runtime_dlls(AveMediaBridgeLabApp)
