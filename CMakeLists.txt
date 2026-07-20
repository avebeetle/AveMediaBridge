cmake_minimum_required(VERSION 3.20)

project(AveMediaBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

get_filename_component(AVEMEDIABRIDGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
set(AVEMEDIABRIDGE_FFMPEG_ROOT
    "${AVEMEDIABRIDGE_ROOT}/ffmpeg"
    CACHE PATH
    "FFmpeg headers, import libraries, and runtime DLL root")
get_filename_component(FFMPEG_ROOT "${AVEMEDIABRIDGE_FFMPEG_ROOT}" ABSOLUTE)
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
    src/Decode/AudioDecodeHelpers.cpp
    src/Decode/AudioPresentationSlice.cpp
    src/Decode/PcmFormat.cpp
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

add_executable(AveMediaBridgeAudioPresentationSliceTests
    tests/AudioPresentationSliceTests.cpp
)

target_link_libraries(AveMediaBridgeAudioPresentationSliceTests PRIVATE AveMediaBridgeCore)
target_include_directories(AveMediaBridgeAudioPresentationSliceTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}/src"
    "${FFMPEG_INCLUDE_DIR}"
)
copy_ffmpeg_runtime_dlls(AveMediaBridgeAudioPresentationSliceTests)

add_test(
    NAME AveMediaBridgeTests.audio_presentation_slice
    COMMAND AveMediaBridgeAudioPresentationSliceTests
)

add_executable(AveMediaBridgeAudioPresentationEvidenceScanTests
    tests/AudioPresentationEvidenceScanTests.cpp
    src/Probe/FrameCountPolicy.cpp
    src/Probe/PacketScan.cpp
)

target_link_libraries(AveMediaBridgeAudioPresentationEvidenceScanTests PRIVATE AveMediaBridgeCore)
target_include_directories(AveMediaBridgeAudioPresentationEvidenceScanTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}/src"
    "${FFMPEG_INCLUDE_DIR}"
)
copy_ffmpeg_runtime_dlls(AveMediaBridgeAudioPresentationEvidenceScanTests)

add_test(
    NAME AveMediaBridgeTests.audio_presentation_evidence_scan
    COMMAND AveMediaBridgeAudioPresentationEvidenceScanTests
)

set(AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_tests_40min_speech/10_mkv_h264_aac_stereo_48000.mkv"
    CACHE FILEPATH
    "External Matroska/AAC CodecDelay regression media")
set(AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_TEST_MASTER
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_tests_40min_speech/_source/speech_40min_master_48000_stereo_f32.wav"
    CACHE FILEPATH
    "External exact PCM master for the Matroska CodecDelay regression")
set(AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_EXPECTED_FRAMES
    115200000
    CACHE STRING
    "Expected decoded presentation frames for the CodecDelay regression media")
set(AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_EXPECTED_SKIP
    1024
    CACHE STRING
    "Expected initial skip frames for the CodecDelay regression media")
set(AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_EXPECTED_PROBE_FRAMES
    115201008
    CACHE STRING
    "Expected raw rounded container frame estimate for the CodecDelay regression media")

add_executable(AveMediaBridgeMatroskaCodecDelaySkipTests
    tests/MatroskaCodecDelaySkipTests.cpp
    src/Probe/FrameCountPolicy.cpp
    src/Probe/PacketScan.cpp
)

target_link_libraries(AveMediaBridgeMatroskaCodecDelaySkipTests PRIVATE
    AveMediaBridge
    AveMediaBridgeCore
    "${FFMPEG_LIB_DIR}/avformat.lib"
    "${FFMPEG_LIB_DIR}/avcodec.lib"
    "${FFMPEG_LIB_DIR}/avutil.lib"
    "${FFMPEG_LIB_DIR}/swresample.lib"
)
target_include_directories(AveMediaBridgeMatroskaCodecDelaySkipTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}"
    "${FFMPEG_INCLUDE_DIR}"
)
copy_ffmpeg_runtime_dlls(AveMediaBridgeMatroskaCodecDelaySkipTests)

add_test(
    NAME AveMediaBridgeTests.matroska_codec_delay_skip
    COMMAND AveMediaBridgeMatroskaCodecDelaySkipTests
        "${AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_TEST_MEDIA}"
        "${AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_TEST_MASTER}"
        "${AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_EXPECTED_FRAMES}"
        "${AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_EXPECTED_SKIP}"
        "${AVEMEDIABRIDGE_MATROSKA_CODEC_DELAY_EXPECTED_PROBE_FRAMES}"
)
set_tests_properties(
    AveMediaBridgeTests.matroska_codec_delay_skip
    PROPERTIES SKIP_RETURN_CODE 77)

set(AVEMEDIABRIDGE_MP4_MP3_PRESENTATION_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_tests_40min_speech/28_mp4_h264_mp3_stereo_44100.mp4"
    CACHE FILEPATH
    "External MP4/MP3 post-scan presentation-authority regression media")

add_executable(AveMediaBridgeMp4Mp3LoadingReadyPresentationTests
    tests/Mp4Mp3LoadingReadyPresentationTests.cpp
    src/Probe/FrameCountPolicy.cpp
    src/Probe/PacketScan.cpp
)

target_link_libraries(AveMediaBridgeMp4Mp3LoadingReadyPresentationTests PRIVATE
    AveMediaBridge
    AveMediaBridgeCore
    "${FFMPEG_LIB_DIR}/avformat.lib"
    "${FFMPEG_LIB_DIR}/avcodec.lib"
    "${FFMPEG_LIB_DIR}/avutil.lib"
    "${FFMPEG_LIB_DIR}/swresample.lib"
)
target_include_directories(AveMediaBridgeMp4Mp3LoadingReadyPresentationTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}"
    "${FFMPEG_INCLUDE_DIR}"
)
copy_ffmpeg_runtime_dlls(AveMediaBridgeMp4Mp3LoadingReadyPresentationTests)

add_test(
    NAME AveMediaBridgeTests.mp4_mp3_loading_ready_presentation
    COMMAND AveMediaBridgeMp4Mp3LoadingReadyPresentationTests
        "${AVEMEDIABRIDGE_MP4_MP3_PRESENTATION_TEST_MEDIA}"
)
set_tests_properties(
    AveMediaBridgeTests.mp4_mp3_loading_ready_presentation
    PROPERTIES SKIP_RETURN_CODE 77)
