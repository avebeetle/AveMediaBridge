# Pinned, private Xiph sources. Neither an additional DLL nor a system codec is required.
include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
set(AVEMEDIABRIDGE_XIPH_ARCHIVES "" CACHE PATH "Optional directory containing the original pinned Xiph .tar.xz files")
function(bridge_fetch_xiph name version hash)
    if(AVEMEDIABRIDGE_XIPH_ARCHIVES)
        set(archive "${AVEMEDIABRIDGE_XIPH_ARCHIVES}/lib${name}-${version}.tar.xz")
    else()
        set(archive "https://downloads.xiph.org/releases/${name}/lib${name}-${version}.tar.xz")
    endif()
    FetchContent_Declare(bridge_${name}
        URL "${archive}" URL_HASH "SHA256=${hash}"
        SOURCE_SUBDIR unused-upstream-targets)
    FetchContent_MakeAvailable(bridge_${name})
    set(${name}_sources "${bridge_${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

bridge_fetch_xiph(ogg 1.3.5 c4d91be36fc8e54deae7575241e03f4211eb102afb3fc0775fbbc1b740016705)
bridge_fetch_xiph(vorbis 1.3.7 b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b)

# Apply the reviewed changes to private build-tree copies. Public headers of
# installed libvorbisfile must never be used with this private callback layout.
find_package(Git REQUIRED)
set(xiph_private "${CMAKE_CURRENT_BINARY_DIR}/xiph-private")
set(xiph_stage "${CMAKE_CURRENT_BINARY_DIR}/xiph-patch")
set(xiph_patch "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xiph/vorbisfile-private.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${xiph_patch}")
foreach(file IN ITEMS lib/vorbisfile.c include/vorbis/vorbisfile.h)
    configure_file("${vorbis_sources}/${file}" "${xiph_stage}/${file}" COPYONLY)
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}"
    "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${xiph_patch}"
    WORKING_DIRECTORY "${xiph_stage}" COMMAND_ERROR_IS_FATAL ANY)
foreach(file IN ITEMS lib/vorbisfile.c include/vorbis/vorbisfile.h)
    configure_file("${xiph_stage}/${file}" "${xiph_private}/${file}" COPYONLY)
endforeach()

set(xiph_vorbis_units mdct smallft block envelope window lsp lpc analysis synthesis psy info
    floor1 floor0 res0 mapping0 registry codebook sharedbook lookup bitrate)
list(TRANSFORM xiph_vorbis_units PREPEND "${vorbis_sources}/lib/")
list(TRANSFORM xiph_vorbis_units APPEND ".c")
add_library(AveBridgePrivateXiph STATIC
    ${xiph_vorbis_units}
    "${ogg_sources}/src/bitwise.c" "${ogg_sources}/src/framing.c"
    "${xiph_private}/lib/vorbisfile.c")
target_include_directories(AveBridgePrivateXiph
    PUBLIC "${xiph_private}/include" "${vorbis_sources}/include" "${vorbis_sources}/include/vorbis" "${ogg_sources}/include"
    PRIVATE "${vorbis_sources}/lib")
target_compile_definitions(AveBridgePrivateXiph PRIVATE _CRT_SECURE_NO_WARNINGS)

add_library(AveBridgeVorbis STATIC
    src/Decode/VorbisInput.cpp src/Decode/VorbisPresentation.cpp src/Decode/VorbisPackets.cpp
    src/Input/StableInputContract.cpp src/Input/MediaInputSource.cpp)
target_include_directories(AveBridgeVorbis PRIVATE "${AVEMEDIABRIDGE_ROOT}/src" "${AVEMEDIABRIDGE_ROOT}/include"
    "${FFMPEG_INCLUDE_DIR}")
target_compile_definitions(AveBridgeVorbis PRIVATE NOMINMAX OV_EXCLUDE_STATIC_CALLBACKS)
target_link_libraries(AveBridgeVorbis PRIVATE AveBridgePrivateXiph)
