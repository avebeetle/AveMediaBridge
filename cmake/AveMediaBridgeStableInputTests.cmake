add_executable(AveMediaBridgeStableInputContractTests
    tests/StableInputContractTests.cpp
    tests/StableInputAbiC.c
    src/Input/StableInputContract.cpp)
target_include_directories(AveMediaBridgeStableInputContractTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}/include"
    "${AVEMEDIABRIDGE_ROOT}/src")
add_test(NAME AveMediaBridgeTests.stable_input_contract
    COMMAND AveMediaBridgeStableInputContractTests)
set_tests_properties(AveMediaBridgeTests.stable_input_contract PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 60)

add_executable(AveMediaBridgeStableAvioInputTests
    tests/StableAvioInputTests.cpp
    src/Input/StableAvioInput.cpp
    src/Input/StableInputContract.cpp)
target_include_directories(AveMediaBridgeStableAvioInputTests PRIVATE
    "${AVEMEDIABRIDGE_ROOT}/include"
    "${AVEMEDIABRIDGE_ROOT}/src"
    "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(AveMediaBridgeStableAvioInputTests PRIVATE
    "${FFMPEG_LIB_DIR}/avformat.lib"
    "${FFMPEG_LIB_DIR}/avcodec.lib"
    "${FFMPEG_LIB_DIR}/avutil.lib")
copy_ffmpeg_runtime_dlls(AveMediaBridgeStableAvioInputTests)
add_test(NAME AveMediaBridgeTests.stable_avio_input
    COMMAND AveMediaBridgeStableAvioInputTests)
set_tests_properties(AveMediaBridgeTests.stable_avio_input PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 60)
