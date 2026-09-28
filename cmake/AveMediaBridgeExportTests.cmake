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
