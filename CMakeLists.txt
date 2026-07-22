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
    src/Probe/FlacStreamInfo.cpp
    src/Probe/FrameCountPolicy.cpp
    src/Probe/AacAudioSpecificConfig.cpp
    src/Probe/EbmlSequentialReader.cpp
    src/Probe/MatroskaAacSequentialPresentation.cpp
    src/Probe/MediaProbeService.cpp
    src/Probe/Mp4Mp3SampleEditTablePresentation.cpp
    src/Probe/Mp3HeaderPresentation.cpp
    src/Probe/NutBoundedTailAuthority.cpp
    src/Probe/OggOpusSequentialPresentation.cpp
    src/Probe/PacketScan.cpp
    src/Probe/PresentationBudgetPolicy.cpp
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

set(AVEMEDIABRIDGE_EA102_VORBIS_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_test_laboratory_v1/exact_authority/valid/vorbis/EA102_ogg_vorbis_48000_63.ogg"
    CACHE FILEPATH
    "External 63-frame Ogg Vorbis exact-total regression media")
set(AVEMEDIABRIDGE_EA120_VORBIS_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_test_laboratory_v1/exact_authority/valid/vorbis/EA120_ogg_vorbis_11025_1023.ogg"
    CACHE FILEPATH
    "External 1023-frame Ogg Vorbis exact-total regression media")
set(AVEMEDIABRIDGE_GOLDEN06_VORBIS_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_tests_40min_speech/06_ogg_vorbis_stereo_44100.ogg"
    CACHE FILEPATH
    "External long Ogg Vorbis Golden regression media")

add_executable(AveMediaBridgeExactTotalPresentationBudgetTests
    tests/ExactTotalPresentationBudgetTests.cpp
    src/Probe/FlacStreamInfo.cpp
    src/Probe/PresentationBudgetPolicy.cpp
)

target_link_libraries(AveMediaBridgeExactTotalPresentationBudgetTests PRIVATE
    AveMediaBridge
    AveMediaBridgeCore
    bcrypt
)
target_include_directories(AveMediaBridgeExactTotalPresentationBudgetTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}"
    "${FFMPEG_INCLUDE_DIR}"
)
copy_ffmpeg_runtime_dlls(AveMediaBridgeExactTotalPresentationBudgetTests)

add_test(
    NAME AveMediaBridgeTests.exact_total_presentation_budget
    COMMAND AveMediaBridgeExactTotalPresentationBudgetTests
        "${AVEMEDIABRIDGE_EA102_VORBIS_TEST_MEDIA}"
        "${AVEMEDIABRIDGE_EA120_VORBIS_TEST_MEDIA}"
        "${AVEMEDIABRIDGE_GOLDEN06_VORBIS_TEST_MEDIA}"
)
set_tests_properties(
    AveMediaBridgeTests.exact_total_presentation_budget
    PROPERTIES SKIP_RETURN_CODE 77)

set(AVEMEDIABRIDGE_EA020_FLAC_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_test_laboratory_v1/exact_authority/valid/pcm_lossless/EA020_mka_flac_16000_257.mka"
    CACHE FILEPATH
    "External MKA/FLAC 257-frame STREAMINFO regression media")
set(AVEMEDIABRIDGE_EA069_FLAC_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_test_laboratory_v1/exact_authority/valid/pcm_lossless/EA069_mka_flac_48000_102400.mka"
    CACHE FILEPATH
    "External MKA/FLAC 102400-frame STREAMINFO regression media")
set(AVEMEDIABRIDGE_EA085_FLAC_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_test_laboratory_v1/exact_authority/valid/pcm_lossless/EA085_mka_flac_48000_103423.mka"
    CACHE FILEPATH
    "External MKA/FLAC 103423-frame STREAMINFO regression media")
set(AVEMEDIABRIDGE_EA094_FLAC_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_test_laboratory_v1/exact_authority/valid/pcm_lossless/EA094_mka_flac_48000_28801023.mka"
    CACHE FILEPATH
    "External MKA/FLAC long STREAMINFO regression media")

add_executable(AveMediaBridgeFlacStreamInfoPresentationAuthorityTests
    tests/FlacStreamInfoPresentationAuthorityTests.cpp
    src/Probe/FlacStreamInfo.cpp
    src/Probe/PresentationBudgetPolicy.cpp
)

target_link_libraries(AveMediaBridgeFlacStreamInfoPresentationAuthorityTests PRIVATE
    AveMediaBridge
    AveMediaBridgeCore
    bcrypt
)
target_include_directories(AveMediaBridgeFlacStreamInfoPresentationAuthorityTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}"
    "${FFMPEG_INCLUDE_DIR}"
)
copy_ffmpeg_runtime_dlls(AveMediaBridgeFlacStreamInfoPresentationAuthorityTests)

add_test(
    NAME AveMediaBridgeTests.flac_streaminfo_presentation_authority
    COMMAND AveMediaBridgeFlacStreamInfoPresentationAuthorityTests
        "${AVEMEDIABRIDGE_EA020_FLAC_TEST_MEDIA}"
        "${AVEMEDIABRIDGE_EA069_FLAC_TEST_MEDIA}"
        "${AVEMEDIABRIDGE_EA085_FLAC_TEST_MEDIA}"
        "${AVEMEDIABRIDGE_EA094_FLAC_TEST_MEDIA}"
)
set_tests_properties(
    AveMediaBridgeTests.flac_streaminfo_presentation_authority
    PROPERTIES SKIP_RETURN_CODE 77)

set(AVEMEDIABRIDGE_EA021_NUT_PCM_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_test_laboratory_v1/exact_authority/valid/pcm_lossless/EA021_nut_pcm_22050_257.nut"
    CACHE FILEPATH
    "External NUT/PCM 257-frame bounded-tail regression media")
set(AVEMEDIABRIDGE_EA119_NUT_PCM_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_test_laboratory_v1/exact_authority/valid/pcm_lossless/EA119_nut_pcm_192000_191999.nut"
    CACHE FILEPATH
    "External NUT/PCM 191999-frame bounded-tail regression media")
set(AVEMEDIABRIDGE_EA136_NUT_PCM_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_test_laboratory_v1/exact_authority/valid/pcm_lossless/EA136_nut_pcm_88200_1058537.nut"
    CACHE FILEPATH
    "External NUT/PCM 1058537-frame bounded-tail regression media")
set(AVEMEDIABRIDGE_NUT_TAIL_RESEARCH_FIXTURE_ROOT
    "${AVEMEDIABRIDGE_ROOT}/../ffmpeglab/research/nut_bounded_tail_authority/fixtures"
    CACHE PATH
    "External read-only NUT bounded-tail failure and multistream fixtures")

add_executable(AveMediaBridgeNutBoundedTailAuthorityTests
    tests/NutBoundedTailAuthorityTests.cpp
    src/Probe/NutBoundedTailAuthority.cpp
)

target_link_libraries(AveMediaBridgeNutBoundedTailAuthorityTests PRIVATE
    AveMediaBridge
    AveMediaBridgeCore
    "${FFMPEG_LIB_DIR}/avformat.lib"
    "${FFMPEG_LIB_DIR}/avcodec.lib"
    "${FFMPEG_LIB_DIR}/avutil.lib"
    bcrypt
)
target_include_directories(AveMediaBridgeNutBoundedTailAuthorityTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}"
    "${FFMPEG_INCLUDE_DIR}"
)
copy_ffmpeg_runtime_dlls(AveMediaBridgeNutBoundedTailAuthorityTests)

add_test(
    NAME AveMediaBridgeTests.nut_bounded_tail_authority
    COMMAND AveMediaBridgeNutBoundedTailAuthorityTests
        "${AVEMEDIABRIDGE_EA021_NUT_PCM_TEST_MEDIA}"
        "${AVEMEDIABRIDGE_EA119_NUT_PCM_TEST_MEDIA}"
        "${AVEMEDIABRIDGE_EA136_NUT_PCM_TEST_MEDIA}"
        "${AVEMEDIABRIDGE_NUT_TAIL_RESEARCH_FIXTURE_ROOT}"
)
set_tests_properties(
    AveMediaBridgeTests.nut_bounded_tail_authority
    PROPERTIES SKIP_RETURN_CODE 77)

set(AVEMEDIABRIDGE_MP3_HEADER_FIXTURE_ROOT
    "${AVEMEDIABRIDGE_ROOT}/../ffmpeglab/research/mp3_header_authority/fixtures"
    CACHE PATH
    "External read-only MP3 header-authority research fixtures")
set(AVEMEDIABRIDGE_REAL_MP3_HEADER_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_tests_40min_speech/02_mp3_stereo_44100.mp3"
    CACHE FILEPATH
    "External real 40-minute standalone MP3 regression media")
set(AVEMEDIABRIDGE_NO_XING_MP3_TEST_MEDIA
    "${AVEMEDIABRIDGE_MP3_HEADER_FIXTURE_ROOT}/no_xing.mp3"
    CACHE FILEPATH
    "External standalone MP3 full-scan fallback regression media")

add_executable(AveMediaBridgeMp3HeaderPresentationAuthorityTests
    tests/Mp3HeaderPresentationAuthorityTests.cpp
    src/Probe/Mp3HeaderPresentation.cpp
)

target_link_libraries(AveMediaBridgeMp3HeaderPresentationAuthorityTests PRIVATE
    AveMediaBridge
    AveMediaBridgeCore
    "${FFMPEG_LIB_DIR}/avformat.lib"
    "${FFMPEG_LIB_DIR}/avcodec.lib"
    "${FFMPEG_LIB_DIR}/avutil.lib"
    bcrypt
)
target_include_directories(AveMediaBridgeMp3HeaderPresentationAuthorityTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}"
    "${FFMPEG_INCLUDE_DIR}"
)
copy_ffmpeg_runtime_dlls(AveMediaBridgeMp3HeaderPresentationAuthorityTests)

add_test(
    NAME AveMediaBridgeTests.mp3_header_presentation_authority
    COMMAND AveMediaBridgeMp3HeaderPresentationAuthorityTests
        "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_test_laboratory_v1"
        "${AVEMEDIABRIDGE_MP3_HEADER_FIXTURE_ROOT}"
        "${AVEMEDIABRIDGE_REAL_MP3_HEADER_TEST_MEDIA}"
        "${AVEMEDIABRIDGE_NO_XING_MP3_TEST_MEDIA}"
        "${AVEMEDIABRIDGE_MP4_MP3_PRESENTATION_TEST_MEDIA}"
)
set_tests_properties(
    AveMediaBridgeTests.mp3_header_presentation_authority
    PROPERTIES SKIP_RETURN_CODE 77)

set(AVEMEDIABRIDGE_MP4_MP3_SAMPLE_TABLE_FIXTURE_ROOT
    "${AVEMEDIABRIDGE_ROOT}/../ffmpeglab/research/mp4_mp3_sample_table_authority/fixtures"
    CACHE PATH
    "External read-only MP4/MP3 sample/edit-table research fixtures")

add_executable(AveMediaBridgeMp4Mp3SampleEditTablePresentationTests
    tests/Mp4Mp3SampleEditTablePresentationTests.cpp
    src/Probe/Mp4Mp3SampleEditTablePresentation.cpp
)
target_link_libraries(AveMediaBridgeMp4Mp3SampleEditTablePresentationTests PRIVATE
    AveMediaBridge
    AveMediaBridgeCore
    "${FFMPEG_LIB_DIR}/avformat.lib"
    "${FFMPEG_LIB_DIR}/avcodec.lib"
    "${FFMPEG_LIB_DIR}/avutil.lib"
)
target_include_directories(AveMediaBridgeMp4Mp3SampleEditTablePresentationTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}"
    "${AVEMEDIABRIDGE_ROOT}/src"
    "${FFMPEG_INCLUDE_DIR}"
)
copy_ffmpeg_runtime_dlls(AveMediaBridgeMp4Mp3SampleEditTablePresentationTests)

add_test(
    NAME AveMediaBridgeTests.mp4_mp3_sample_edit_table_presentation
    COMMAND AveMediaBridgeMp4Mp3SampleEditTablePresentationTests
        "${AVEMEDIABRIDGE_MP4_MP3_SAMPLE_TABLE_FIXTURE_ROOT}"
        "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_test_laboratory_v1"
        "${AVEMEDIABRIDGE_MP4_MP3_PRESENTATION_TEST_MEDIA}"
)
set_tests_properties(
    AveMediaBridgeTests.mp4_mp3_sample_edit_table_presentation
    PROPERTIES SKIP_RETURN_CODE 77)

set(AVEMEDIABRIDGE_OGG_OPUS_SEQUENTIAL_FIXTURE_ROOT
    "${AVEMEDIABRIDGE_ROOT}/../ffmpeglab/research/ogg_opus_sequential_exact_scan/fixtures"
    CACHE PATH
    "External read-only Ogg Opus sequential-authority research fixtures")
set(AVEMEDIABRIDGE_GOLDEN07_OPUS_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_tests_40min_speech/07_ogg_opus_stereo_48000.opus"
    CACHE FILEPATH
    "External long Ogg Opus sequential-authority regression media")

add_executable(AveMediaBridgeOggOpusSequentialPresentationTests
    tests/OggOpusSequentialPresentationTests.cpp
    src/Probe/OggOpusSequentialPresentation.cpp
)

target_link_libraries(AveMediaBridgeOggOpusSequentialPresentationTests PRIVATE
    AveMediaBridge
    AveMediaBridgeCore
    "${FFMPEG_LIB_DIR}/avformat.lib"
    "${FFMPEG_LIB_DIR}/avcodec.lib"
    "${FFMPEG_LIB_DIR}/avutil.lib"
)
target_include_directories(AveMediaBridgeOggOpusSequentialPresentationTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}"
    "${AVEMEDIABRIDGE_ROOT}/src"
    "${FFMPEG_INCLUDE_DIR}"
)
copy_ffmpeg_runtime_dlls(AveMediaBridgeOggOpusSequentialPresentationTests)

add_test(
    NAME AveMediaBridgeTests.ogg_opus_sequential_presentation
    COMMAND AveMediaBridgeOggOpusSequentialPresentationTests
        "${AVEMEDIABRIDGE_OGG_OPUS_SEQUENTIAL_FIXTURE_ROOT}"
        "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_test_laboratory_v1"
        "${AVEMEDIABRIDGE_GOLDEN07_OPUS_TEST_MEDIA}"
)
set_tests_properties(
    AveMediaBridgeTests.ogg_opus_sequential_presentation
    PROPERTIES SKIP_RETURN_CODE 77)

set(AVEMEDIABRIDGE_MATROSKA_AAC_RESEARCH_FIXTURE_ROOT
    "${AVEMEDIABRIDGE_ROOT}/../ffmpeglab/research/matroska_aac_exact_authority/fixtures"
    CACHE PATH
    "External read-only Matroska AAC exact-authority research fixtures")
set(AVEMEDIABRIDGE_MATROSKA_AAC_IMPLEMENTATION_FIXTURE_ROOT
    "${AVEMEDIABRIDGE_ROOT}/../ffmpeglab/research/matroska_aac_sequential_exact_scan_fix/fixtures"
    CACHE PATH
    "External read-only Matroska AAC implementation fixtures")
set(AVEMEDIABRIDGE_GOLDEN10_AAC_TEST_MEDIA
    "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_tests_40min_speech/10_mkv_h264_aac_stereo_48000.mkv"
    CACHE FILEPATH
    "External long Matroska AAC regression media")

add_executable(AveMediaBridgeMatroskaAacSequentialPresentationTests
    tests/MatroskaAacSequentialPresentationTests.cpp
    src/Probe/AacAudioSpecificConfig.cpp
    src/Probe/EbmlSequentialReader.cpp
    src/Probe/MatroskaAacSequentialPresentation.cpp
    src/Probe/PresentationBudgetPolicy.cpp
    src/Probe/FlacStreamInfo.cpp
)
target_link_libraries(AveMediaBridgeMatroskaAacSequentialPresentationTests PRIVATE
    AveMediaBridge
    AveMediaBridgeCore
    "${FFMPEG_LIB_DIR}/avformat.lib"
    "${FFMPEG_LIB_DIR}/avcodec.lib"
    "${FFMPEG_LIB_DIR}/avutil.lib"
)
target_include_directories(AveMediaBridgeMatroskaAacSequentialPresentationTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}"
    "${AVEMEDIABRIDGE_ROOT}/src"
    "${FFMPEG_INCLUDE_DIR}"
)
copy_ffmpeg_runtime_dlls(AveMediaBridgeMatroskaAacSequentialPresentationTests)

add_test(
    NAME AveMediaBridgeTests.matroska_aac_sequential_presentation
    COMMAND AveMediaBridgeMatroskaAacSequentialPresentationTests
        "${AVEMEDIABRIDGE_MATROSKA_AAC_RESEARCH_FIXTURE_ROOT}"
        "${AVEMEDIABRIDGE_MATROSKA_AAC_IMPLEMENTATION_FIXTURE_ROOT}"
        "${AVEMEDIABRIDGE_ROOT}/../AveMediaBridge_test_laboratory_v1"
        "${AVEMEDIABRIDGE_GOLDEN10_AAC_TEST_MEDIA}"
)
set_tests_properties(
    AveMediaBridgeTests.matroska_aac_sequential_presentation
    PROPERTIES SKIP_RETURN_CODE 77)
