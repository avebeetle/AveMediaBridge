add_executable(AveMediaBridgeReaderImportAbiTests tests/ReaderImportAbiTests.cpp tests/ReaderImportAbiC.c)
target_include_directories(AveMediaBridgeReaderImportAbiTests PRIVATE "${AVEMEDIABRIDGE_ROOT}/include")
target_link_libraries(AveMediaBridgeReaderImportAbiTests PRIVATE AveMediaBridge)
set_property(TARGET AveMediaBridgeReaderImportAbiTests PROPERTY C_STANDARD 11)
copy_ffmpeg_runtime_dlls(AveMediaBridgeReaderImportAbiTests)
add_test(NAME AveMediaBridgeTests.reader_import_abi COMMAND AveMediaBridgeReaderImportAbiTests)
set_tests_properties(AveMediaBridgeTests.reader_import_abi PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)

add_executable(AveMediaBridgeReaderMediaFactsTests
    tests/ReaderMediaFactsTests.cpp tests/ReaderMediaFactsAbiC.c
    src/Probe/ReaderMediaFacts.cpp src/Utils/JsonUtils.cpp)
target_include_directories(AveMediaBridgeReaderMediaFactsTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}/include" "${AVEMEDIABRIDGE_ROOT}/src" "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(AveMediaBridgeReaderMediaFactsTests PRIVATE AveMediaBridge
    "${FFMPEG_LIB_DIR}/avformat.lib" "${FFMPEG_LIB_DIR}/avcodec.lib" "${FFMPEG_LIB_DIR}/avutil.lib")
set_property(TARGET AveMediaBridgeReaderMediaFactsTests PROPERTY C_STANDARD 11)
copy_ffmpeg_runtime_dlls(AveMediaBridgeReaderMediaFactsTests)
add_test(NAME AveMediaBridgeTests.reader_media_facts COMMAND AveMediaBridgeReaderMediaFactsTests)
set(AVEMEDIABRIDGE_READER_FACTS_DATA_ROOT "${CMAKE_CURRENT_BINARY_DIR}/reader-facts-data" CACHE PATH
    "Isolated data root for reader media facts tests")
file(MAKE_DIRECTORY "${AVEMEDIABRIDGE_READER_FACTS_DATA_ROOT}")
set_tests_properties(AveMediaBridgeTests.reader_media_facts PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT "AVEVOICE_DATA_ROOT=${AVEMEDIABRIDGE_READER_FACTS_DATA_ROOT};TEMP=${AVEMEDIABRIDGE_READER_FACTS_DATA_ROOT};TMP=${AVEMEDIABRIDGE_READER_FACTS_DATA_ROOT}")
set(AVEMEDIABRIDGE_READER_FACTS_FIXTURE_ROOT "" CACHE PATH
    "Explicit generated cover/single/two-video fixture root")
if(AVEMEDIABRIDGE_READER_FACTS_FIXTURE_ROOT)
    foreach(fixture IN ITEMS reader_cover_only_aac.mp4 reader_single_video_aac.mp4
            reader_two_video_aac.mp4)
        if(NOT EXISTS "${AVEMEDIABRIDGE_READER_FACTS_FIXTURE_ROOT}/${fixture}")
            message(FATAL_ERROR "Missing media facts fixture: ${fixture}")
        endif()
    endforeach()
    add_test(NAME AveMediaBridgeTests.reader_media_facts_real
        COMMAND AveMediaBridgeReaderMediaFactsTests "${AVEMEDIABRIDGE_READER_FACTS_FIXTURE_ROOT}")
    set_tests_properties(AveMediaBridgeTests.reader_media_facts_real PROPERTIES
        RUN_SERIAL TRUE TIMEOUT 180
        ENVIRONMENT "AVEVOICE_DATA_ROOT=${AVEMEDIABRIDGE_READER_FACTS_DATA_ROOT};TEMP=${AVEMEDIABRIDGE_READER_FACTS_DATA_ROOT};TMP=${AVEMEDIABRIDGE_READER_FACTS_DATA_ROOT}")
endif()

set(AVEMEDIABRIDGE_READER_FIXTURE_ROOT "" CACHE PATH
    "Existing absolute directory of generated reader MP4/AAC fixtures")
set(AVEMEDIABRIDGE_READER_LAB_ROOT "" CACHE PATH
    "Existing absolute directory of read-only laboratory media")
if(AVEMEDIABRIDGE_READER_LAB_ROOT AND
   (NOT IS_ABSOLUTE "${AVEMEDIABRIDGE_READER_LAB_ROOT}" OR
    NOT IS_DIRECTORY "${AVEMEDIABRIDGE_READER_LAB_ROOT}"))
    message(FATAL_ERROR "Reader laboratory root must be an existing absolute directory")
endif()

if(AVEMEDIABRIDGE_READER_FIXTURE_ROOT)
    if(NOT IS_ABSOLUTE "${AVEMEDIABRIDGE_READER_FIXTURE_ROOT}" OR
       NOT EXISTS "${AVEMEDIABRIDGE_READER_FIXTURE_ROOT}/reader_demux_aac.m4a")
        message(FATAL_ERROR "Reader fixture root must be absolute and contain reader_demux_aac.m4a")
    endif()

    add_executable(AveMediaBridgeMediaInputDemuxTests
        tests/MediaInputDemuxTests.cpp
        src/Input/MediaInputSource.cpp
        src/Input/DemuxSession.cpp
        src/Input/StableAvioInput.cpp
        src/Input/StableInputContract.cpp)
    target_include_directories(AveMediaBridgeMediaInputDemuxTests PRIVATE
        "${AVEMEDIABRIDGE_ROOT}/include"
        "${AVEMEDIABRIDGE_ROOT}/src"
        "${FFMPEG_INCLUDE_DIR}")
    target_link_libraries(AveMediaBridgeMediaInputDemuxTests PRIVATE
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib")
    copy_ffmpeg_runtime_dlls(AveMediaBridgeMediaInputDemuxTests)
    add_test(NAME AveMediaBridgeTests.media_input_demux
        COMMAND AveMediaBridgeMediaInputDemuxTests
            "${AVEMEDIABRIDGE_READER_FIXTURE_ROOT}/reader_demux_aac.m4a")
    set_tests_properties(AveMediaBridgeTests.media_input_demux PROPERTIES
        RUN_SERIAL TRUE TIMEOUT 60)

    if(NOT AVEMEDIABRIDGE_READER_LAB_ROOT)
        message(FATAL_ERROR "Reader probe parity requires AVEMEDIABRIDGE_READER_LAB_ROOT")
    endif()
    foreach(fixture IN ITEMS reader_stereo_front_aac.mp4
            reader_stereo_tail_aac.mp4 reader_two_aac_default_second.mp4
            reader_mp4_mp3_control.mp4 reader_mp4_alac_control.m4a
            reader_mp4_no_audio.mp4)
        if(NOT EXISTS "${AVEMEDIABRIDGE_READER_FIXTURE_ROOT}/${fixture}")
            message(FATAL_ERROR "Missing required reader fixture: ${fixture}")
        endif()
    endforeach()
    foreach(fixture IN ITEMS
            exact_authority/valid/aac/EA035_m4a_aac_96000_10241.m4a
            exact_authority/valid/aac/EA127_mp4_aac_88200_88199.mp4
            exact_authority/valid/aac/EA098_m4a_aac_48000_1.m4a
            av_sync/valid/mp4/AV018_nonzero_audio_media_time_mp4.mp4
            av_sync/valid/mov/AV019_empty_edit_before_audio_mov.mov
            exact_authority/valid/pcm_lossless/EA002_wav_s16_16000_257.wav)
        if(NOT EXISTS "${AVEMEDIABRIDGE_READER_LAB_ROOT}/${fixture}")
            message(FATAL_ERROR "Missing required reader laboratory fixture: ${fixture}")
        endif()
    endforeach()
    set(_reader_probe_sources ${AVEMEDIABRIDGE_DLL_SOURCES})
    list(FILTER _reader_probe_sources EXCLUDE REGEX "^src/Dll/")
    list(FILTER _reader_probe_sources EXCLUDE REGEX "^src/Export/")
    add_executable(AveMediaBridgeReaderProbeTests
        tests/ReaderProbeParityTests.cpp
        ${_reader_probe_sources})
    target_link_libraries(AveMediaBridgeReaderProbeTests PRIVATE AveMediaBridgeCore)
    target_compile_definitions(AveMediaBridgeReaderProbeTests PRIVATE
        AVEMEDIABRIDGE_TEST_ONLY)
    target_include_directories(AveMediaBridgeReaderProbeTests PRIVATE
        "${AVEMEDIABRIDGE_ROOT}/include"
        "${AVEMEDIABRIDGE_ROOT}/src"
        "${FFMPEG_INCLUDE_DIR}")
    copy_ffmpeg_runtime_dlls(AveMediaBridgeReaderProbeTests)
    add_test(NAME AveMediaBridgeTests.reader_probe_parity
        COMMAND AveMediaBridgeReaderProbeTests
            "${AVEMEDIABRIDGE_READER_FIXTURE_ROOT}"
            "${AVEMEDIABRIDGE_READER_LAB_ROOT}")
    set_tests_properties(AveMediaBridgeTests.reader_probe_parity PROPERTIES
        RUN_SERIAL TRUE TIMEOUT 180)
    add_executable(AveMediaBridgeReaderImportTests tests/ReaderImportParityTests.cpp)
    target_include_directories(AveMediaBridgeReaderImportTests PRIVATE "${AVEMEDIABRIDGE_ROOT}/include")
    target_link_libraries(AveMediaBridgeReaderImportTests PRIVATE AveMediaBridge)
    copy_ffmpeg_runtime_dlls(AveMediaBridgeReaderImportTests)
    add_test(NAME AveMediaBridgeTests.reader_mp4_aac_parity COMMAND AveMediaBridgeReaderImportTests
        "${AVEMEDIABRIDGE_READER_FIXTURE_ROOT}" "${AVEMEDIABRIDGE_READER_LAB_ROOT}")
    set_tests_properties(AveMediaBridgeTests.reader_mp4_aac_parity PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
endif()
