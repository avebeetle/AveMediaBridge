add_library(AveMediaBridgeExportCHeaderCheck OBJECT tests/ExportAbiCConsumer.c)
target_include_directories(AveMediaBridgeExportCHeaderCheck PRIVATE "${AVEMEDIABRIDGE_ROOT}/include")

add_executable(AveMediaBridgeExportAbiTests tests/ExportAbiTest.cpp)
target_link_libraries(AveMediaBridgeExportAbiTests PRIVATE AveMediaBridge)
target_include_directories(AveMediaBridgeExportAbiTests PRIVATE "${AVEMEDIABRIDGE_ROOT}/include")
copy_ffmpeg_runtime_dlls(AveMediaBridgeExportAbiTests)
add_test(NAME AveMediaBridgeTests.export_abi COMMAND AveMediaBridgeExportAbiTests)

add_executable(AveMediaBridgeExportJobStateTests
    tests/ExportJobStateTest.cpp
    src/Export/StreamingExportJob.cpp)
target_include_directories(AveMediaBridgeExportJobStateTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}/include" "${AVEMEDIABRIDGE_ROOT}/src")
add_test(NAME AveMediaBridgeTests.export_state COMMAND AveMediaBridgeExportJobStateTests)

add_executable(AveMediaBridgeFloatWavTests tests/FloatWavWriterTest.cpp
    src/Export/FfmpegFloatWavWriter.cpp src/Export/ExportScratchIo.cpp src/Export/StreamingExportJob.cpp)
target_link_libraries(AveMediaBridgeFloatWavTests PRIVATE AveMediaBridge
    "${FFMPEG_LIB_DIR}/avformat.lib" "${FFMPEG_LIB_DIR}/avcodec.lib" "${FFMPEG_LIB_DIR}/avutil.lib")
target_include_directories(AveMediaBridgeFloatWavTests PRIVATE "${AVEMEDIABRIDGE_ROOT}/include"
    "${AVEMEDIABRIDGE_ROOT}/src" "${FFMPEG_INCLUDE_DIR}")
copy_ffmpeg_runtime_dlls(AveMediaBridgeFloatWavTests)
add_test(NAME AveMediaBridgeTests.export_float_wav COMMAND AveMediaBridgeFloatWavTests)

add_executable(AveMediaBridgeExportScratchTests tests/ExportScratchIoTest.cpp src/Export/ExportScratchIo.cpp)
target_link_libraries(AveMediaBridgeExportScratchTests PRIVATE AveMediaBridge
    "${FFMPEG_LIB_DIR}/avformat.lib" "${FFMPEG_LIB_DIR}/avutil.lib")
target_include_directories(AveMediaBridgeExportScratchTests PRIVATE "${AVEMEDIABRIDGE_ROOT}/include"
    "${AVEMEDIABRIDGE_ROOT}/src" "${FFMPEG_INCLUDE_DIR}")
copy_ffmpeg_runtime_dlls(AveMediaBridgeExportScratchTests)
add_test(NAME AveMediaBridgeTests.export_scratch COMMAND AveMediaBridgeExportScratchTests)
