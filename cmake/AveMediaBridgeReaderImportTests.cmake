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
endif()
