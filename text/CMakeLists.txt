# twidget::text (twidget_text, namespace twtext): fonts and text measurement behind TwidgetCore's Backend, shared by
# twidget_flutter's cards and hosts that lay out documents of their own (an EPUB reader). HarfBuzz for shaping and the
# font tables; the system's font matcher: fontconfig on Linux, on Android the NDK's (AFontMatcher, looked up at run
# time) and /system/etc/fonts.xml.
#
# HarfBuzz: on Android (or with TWIDGET_TEXT_BUNDLED_HARFBUZZ) built from source, from a checkout given in
# TWIDGET_TEXT_HARFBUZZ (a CMake or environment variable) or the pinned release fetched; elsewhere the system's
# (pkg-config). No FreeType: metrics come from the font tables.
#
# Glyph outlines for GPU text (encode_glyph_outline): HarfBuzz's hb-gpu (14.2 and later), built with the bundled
# HarfBuzz; with the system's, when its harfbuzz-gpu library is installed. Without it the outlines are unavailable and
# a painter draws text as before.
#
# Added by twidget's top-level CMakeLists.txt; a host project adds it the same way (add_subdirectory(<this> text)).
# Needs the core's headers (core/src: backend.h, canvas_ops.h, display_list.h), nothing compiled.
cmake_minimum_required(VERSION 3.22)
if(TARGET twidget_text)
    return()
endif()

set(TWT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(APPLE)
    set(TWIDGET_TEXT_BUNDLED_HARFBUZZ ON CACHE BOOL "Build HarfBuzz from source on desktop too" FORCE)
endif()
set(TWIDGET_TEXT_HARFBUZZ_VERSION 15.1.0)
set(TWIDGET_TEXT_HARFBUZZ_SHA256 "0fc7c565443a5f4be7da65ac5ee5bd737d9b158dc908708b7689a0a5796b0c8d" CACHE STRING "SHA-256 of the HarfBuzz release tarball fetched when no checkout is given")
option(TWIDGET_TEXT_BUNDLED_HARFBUZZ "Build HarfBuzz from source on desktop too" OFF)

if(ANDROID OR TWIDGET_TEXT_BUNDLED_HARFBUZZ)
    if(NOT TWIDGET_TEXT_HARFBUZZ AND DEFINED ENV{TWIDGET_TEXT_HARFBUZZ})
        set(TWIDGET_TEXT_HARFBUZZ "$ENV{TWIDGET_TEXT_HARFBUZZ}")
    endif()
    if(NOT TWIDGET_TEXT_HARFBUZZ)
        include(FetchContent)
        FetchContent_Declare(twt_harfbuzz_src
            URL https://github.com/harfbuzz/harfbuzz/releases/download/${TWIDGET_TEXT_HARFBUZZ_VERSION}/harfbuzz-${TWIDGET_TEXT_HARFBUZZ_VERSION}.tar.xz
            URL_HASH SHA256=${TWIDGET_TEXT_HARFBUZZ_SHA256})
        FetchContent_Populate(twt_harfbuzz_src)
        set(TWIDGET_TEXT_HARFBUZZ "${twt_harfbuzz_src_SOURCE_DIR}")
    endif()
    if(NOT TARGET tw_harfbuzz)
        # Compiled through a one-line file in the build tree: a source outside it gets an object path that repeats
        # the source's absolute path, too long for the NDK's ninja on deep checkouts.
        # hb-gpu is not in the harfbuzz.cc amalgamation: its two sources (outlines; no COLR paint) are units of their own.
        set(_twt_hb_units)
        set(_twt_hb_sources harfbuzz.cc)
        if(EXISTS "${TWIDGET_TEXT_HARFBUZZ}/src/hb-gpu.h")
            list(APPEND _twt_hb_sources hb-gpu.cc hb-gpu-draw.cc)
        endif()
        foreach(_twt_src IN LISTS _twt_hb_sources)
            set(_twt_hb_unit "${CMAKE_CURRENT_BINARY_DIR}/tw_${_twt_src}")
            set(_twt_hb_line "#include \"${_twt_src}\"\n")
            set(_twt_hb_old "")
            if(EXISTS "${_twt_hb_unit}")
                file(READ "${_twt_hb_unit}" _twt_hb_old)
            endif()
            if(NOT "${_twt_hb_old}" STREQUAL "${_twt_hb_line}")
                file(WRITE "${_twt_hb_unit}" "${_twt_hb_line}")
            endif()
            list(APPEND _twt_hb_units "${_twt_hb_unit}")
        endforeach()
        add_library(tw_harfbuzz STATIC ${_twt_hb_units})
        target_include_directories(tw_harfbuzz PUBLIC "${TWIDGET_TEXT_HARFBUZZ}/src")
        target_compile_definitions(tw_harfbuzz PRIVATE HAVE_MMAP HAVE_SYS_MMAN_H HAVE_UNISTD_H HAVE_PTHREAD)
        target_compile_options(tw_harfbuzz PRIVATE -O2 -w -fno-exceptions -fno-rtti -fvisibility=hidden)
        set_target_properties(tw_harfbuzz PROPERTIES POSITION_INDEPENDENT_CODE ON)
        if(EXISTS "${TWIDGET_TEXT_HARFBUZZ}/src/hb-gpu.h")
            target_compile_definitions(tw_harfbuzz INTERFACE TWT_HB_GPU=1)
        endif()
    endif()
    if(APPLE)
        add_library(twt_system INTERFACE)
        target_link_libraries(twt_system INTERFACE tw_harfbuzz "-framework CoreText" "-framework CoreFoundation" z)
    endif()
    if(NOT TARGET twt_system)
    add_library(twt_system INTERFACE)
    if(ANDROID)
        target_link_libraries(twt_system INTERFACE tw_harfbuzz android log z dl)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(TWT_FC REQUIRED IMPORTED_TARGET fontconfig zlib)
        target_link_libraries(twt_system INTERFACE tw_harfbuzz PkgConfig::TWT_FC)
    endif()
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TWT_TEXT REQUIRED IMPORTED_TARGET harfbuzz fontconfig zlib)
    pkg_check_modules(TWT_HB_GPU QUIET IMPORTED_TARGET harfbuzz-gpu)
    add_library(twt_system INTERFACE)
    target_link_libraries(twt_system INTERFACE PkgConfig::TWT_TEXT)
    if(TWT_HB_GPU_FOUND)
        target_link_libraries(twt_system INTERFACE PkgConfig::TWT_HB_GPU)
        target_compile_definitions(twt_system INTERFACE TWT_HB_GPU=1)
    endif()
endif()

# Book fonts (add_book_font: an EPUB's @font-face): every file goes through the OpenType Sanitizer, as browsers do with
# web fonts (Chromium, Firefox), which also turns WOFF (zlib) and WOFF2 (google/woff2, Brotli) into plain OpenType.
# Fetched at pinned releases, SHA-256 checked (the hashes OTS's own meson wraps give), or checkouts given in
# TWIDGET_TEXT_OTS / TWIDGET_TEXT_WOFF2 / TWIDGET_TEXT_BROTLI (CMake or environment variables). Only the decoders are
# built: Brotli's decoder, woff2's decoder, OTS without Graphite (HarfBuzz here shapes no Graphite; OTS drops those
# tables).
set(TWIDGET_TEXT_OTS_VERSION 9.3.0)
set(TWIDGET_TEXT_OTS_SHA256 "23814f8e90ee77379f54e86a012c09bba2d133940e5257546b29cf087a73beec" CACHE STRING "SHA-256 of the OTS release tarball")
set(TWIDGET_TEXT_WOFF2_VERSION 1.0.2)
set(TWIDGET_TEXT_WOFF2_SHA256 "add272bb09e6384a4833ffca4896350fdb16e0ca22df68c0384773c67a175594" CACHE STRING "SHA-256 of the woff2 source tarball")
set(TWIDGET_TEXT_BROTLI_VERSION 1.1.0)
set(TWIDGET_TEXT_BROTLI_SHA256 "e720a6ca29428b803f4ad165371771f5398faba397edf6778837a18599ea13ff" CACHE STRING "SHA-256 of the Brotli source tarball")
foreach(_twt_dep OTS WOFF2 BROTLI)
    if(NOT TWIDGET_TEXT_${_twt_dep} AND DEFINED ENV{TWIDGET_TEXT_${_twt_dep}})
        set(TWIDGET_TEXT_${_twt_dep} "$ENV{TWIDGET_TEXT_${_twt_dep}}")
    endif()
endforeach()
include(FetchContent)
if(NOT TWIDGET_TEXT_OTS)
    FetchContent_Declare(twt_ots_src
        URL https://github.com/khaledhosny/ots/releases/download/v${TWIDGET_TEXT_OTS_VERSION}/ots-${TWIDGET_TEXT_OTS_VERSION}.tar.xz
        URL_HASH SHA256=${TWIDGET_TEXT_OTS_SHA256})
    FetchContent_Populate(twt_ots_src)
    set(TWIDGET_TEXT_OTS "${twt_ots_src_SOURCE_DIR}")
endif()
if(NOT TWIDGET_TEXT_WOFF2)
    FetchContent_Declare(twt_woff2_src
        URL https://github.com/google/woff2/archive/v${TWIDGET_TEXT_WOFF2_VERSION}.tar.gz
        URL_HASH SHA256=${TWIDGET_TEXT_WOFF2_SHA256})
    FetchContent_Populate(twt_woff2_src)
    set(TWIDGET_TEXT_WOFF2 "${twt_woff2_src_SOURCE_DIR}")
endif()
if(NOT TWIDGET_TEXT_BROTLI)
    FetchContent_Declare(twt_brotli_src
        URL https://github.com/google/brotli/archive/v${TWIDGET_TEXT_BROTLI_VERSION}.tar.gz
        URL_HASH SHA256=${TWIDGET_TEXT_BROTLI_SHA256})
    FetchContent_Populate(twt_brotli_src)
    set(TWIDGET_TEXT_BROTLI "${twt_brotli_src_SOURCE_DIR}")
endif()

# Each source compiled through a one-line file in the build tree (as tw_harfbuzz: short object paths).
function(twt_units out prefix dir)
    set(_units)
    foreach(_src IN LISTS ARGN)
        string(MAKE_C_IDENTIFIER "${_src}" _id)
        get_filename_component(_ext "${_src}" LAST_EXT)
        set(_unit "${CMAKE_CURRENT_BINARY_DIR}/${prefix}_${_id}${_ext}")
        set(_line "#include \"${dir}/${_src}\"\n")
        set(_old "")
        if(EXISTS "${_unit}")
            file(READ "${_unit}" _old)
        endif()
        if(NOT "${_old}" STREQUAL "${_line}")
            file(WRITE "${_unit}" "${_line}")
        endif()
        list(APPEND _units "${_unit}")
    endforeach()
    set(${out} ${_units} PARENT_SCOPE)
endfunction()

if(NOT TARGET twt_ots)
    twt_units(_twt_brotli twt_brotli "${TWIDGET_TEXT_BROTLI}/c"
        common/constants.c common/context.c common/dictionary.c common/platform.c common/shared_dictionary.c
        common/transform.c dec/bit_reader.c dec/decode.c dec/huffman.c dec/state.c)
    twt_units(_twt_woff2 twt_woff2 "${TWIDGET_TEXT_WOFF2}/src"
        table_tags.cc variable_length.cc woff2_common.cc woff2_dec.cc woff2_out.cc)
    file(GLOB _twt_ots_all RELATIVE "${TWIDGET_TEXT_OTS}/src" "${TWIDGET_TEXT_OTS}/src/*.cc")
    list(REMOVE_ITEM _twt_ots_all feat.cc glat.cc gloc.cc sile.cc silf.cc sill.cc)  # Graphite
    twt_units(_twt_ots twt_ots "${TWIDGET_TEXT_OTS}/src" ${_twt_ots_all})
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/twt_ots_config/config.h" "#define OTS_COLR_CYCLE_CHECK 1\n#define OTS_SYNTHESIZE_MISSING_GVAR 1\n")
    add_library(twt_ots STATIC ${_twt_brotli} ${_twt_woff2} ${_twt_ots})
    target_include_directories(twt_ots
        PUBLIC "${TWIDGET_TEXT_OTS}/include"
        PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/twt_ots_config" "${TWIDGET_TEXT_OTS}/src" "${TWIDGET_TEXT_WOFF2}/include"
                "${TWIDGET_TEXT_WOFF2}/src" "${TWIDGET_TEXT_BROTLI}/c/include")
    target_compile_definitions(twt_ots PRIVATE HAVE_CONFIG_H)
    target_compile_options(twt_ots PRIVATE -O2 -w -fvisibility=hidden)
    target_link_libraries(twt_ots PRIVATE twt_system)
    set_target_properties(twt_ots PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

add_library(twidget_text STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/text_backend.cpp")
add_library(twidget::text ALIAS twidget_text)
target_include_directories(twidget_text PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${TWT_ROOT}/core/src")
target_link_libraries(twidget_text PUBLIC twt_system PRIVATE twt_ots)
set_target_properties(twidget_text PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(twidget_text PUBLIC cxx_std_20)
target_compile_options(twidget_text PRIVATE -Wall -Wextra -Wno-unused-parameter)
