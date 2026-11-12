option(AVEMEDIABRIDGE_ENABLE_ASAN "Instrument isolated x64 MSVC RelWithDebInfo builds" OFF)
if(NOT AVEMEDIABRIDGE_ENABLE_ASAN)
    return()
endif()

if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR
   NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "AVEMEDIABRIDGE_ENABLE_ASAN requires the x64 MSVC C and C++ toolset")
endif()

# Debug introduces /RTC by default. Generate only the supported audit configuration.
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES RelWithDebInfo)
elseif(NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    message(FATAL_ERROR "AVEMEDIABRIDGE_ENABLE_ASAN requires -DCMAKE_BUILD_TYPE=RelWithDebInfo")
endif()

set(_ambe_asan_compile_flags
    "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_C_FLAGS_RELWITHDEBINFO} ${CMAKE_CXX_FLAGS_RELWITHDEBINFO} $ENV{CL} $ENV{_CL_}")
if(_ambe_asan_compile_flags MATCHES "(^|[ \t\"])[/-](RTC[^ \t\"]*|ZI|Y[cu][^ \t\"]*)([ \t\"]|$)")
    message(FATAL_ERROR "AVEMEDIABRIDGE_ENABLE_ASAN is incompatible with /RTC, /ZI, or precompiled headers")
endif()
unset(_ambe_asan_compile_flags)

get_filename_component(_ambe_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
if(NOT EXISTS "${_ambe_compiler_dir}/clang_rt.asan_dynamic-x86_64.dll")
    message(FATAL_ERROR "Install the matching x64 MSVC AddressSanitizer runtime in ${_ambe_compiler_dir}")
endif()
unset(_ambe_compiler_dir)

set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
add_compile_options(/fsanitize=address /Zi)
add_link_options(/INCREMENTAL:NO)

set(AVEMEDIABRIDGE_ASAN_PROBE_SOURCE "" CACHE FILEPATH
    "Optional standalone AddressSanitizer probe source supplied by the audit")
if(AVEMEDIABRIDGE_ASAN_PROBE_SOURCE)
    add_executable(AveMediaBridgeAddressSanitizerProbe EXCLUDE_FROM_ALL "${AVEMEDIABRIDGE_ASAN_PROBE_SOURCE}")
endif()
