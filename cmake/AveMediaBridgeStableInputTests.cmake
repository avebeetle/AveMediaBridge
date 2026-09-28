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
