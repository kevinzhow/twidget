# twidget::text (twidget_text, namespace twtext): fonts and text measurement behind TwidgetCore's Backend, shared by
# twidget_flutter's cards and hosts that lay out documents of their own (an EPUB reader). HarfBuzz for shaping and the
# font tables; the system's font matcher: fontconfig on Linux, on Android the NDK's (AFontMatcher, looked up at run
# time) and /system/etc/fonts.xml.
#
# HarfBuzz: on Android (or with TWIDGET_TEXT_BUNDLED_HARFBUZZ) built from source, from a checkout given in
# TWIDGET_TEXT_HARFBUZZ (a CMake or environment variable) or the pinned release fetched; elsewhere the system's
# (pkg-config). No FreeType: metrics come from the font tables.
#
# Added by twidget's top-level CMakeLists.txt; a host project adds it the same way (add_subdirectory(<this> text)).
# Needs the core's headers (core/src: backend.h, canvas_ops.h, display_list.h), nothing compiled.
cmake_minimum_required(VERSION 3.22)
if(TARGET twidget_text)
    return()
endif()

set(TWT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(TWIDGET_TEXT_HARFBUZZ_SHA256 "6f6db164359a2da5a84ef826615b448b33e6306067ad829d85d5b0bf936f1bb8" CACHE STRING "SHA-256 of the HarfBuzz release tarball fetched when no checkout is given")
option(TWIDGET_TEXT_BUNDLED_HARFBUZZ "Build HarfBuzz from source on desktop too" OFF)

if(ANDROID OR TWIDGET_TEXT_BUNDLED_HARFBUZZ)
    if(NOT TWIDGET_TEXT_HARFBUZZ AND DEFINED ENV{TWIDGET_TEXT_HARFBUZZ})
        set(TWIDGET_TEXT_HARFBUZZ "$ENV{TWIDGET_TEXT_HARFBUZZ}")
    endif()
    if(NOT TWIDGET_TEXT_HARFBUZZ)
        include(FetchContent)
        FetchContent_Declare(twt_harfbuzz_src
            URL https://github.com/harfbuzz/harfbuzz/releases/download/12.3.2/harfbuzz-12.3.2.tar.xz
            URL_HASH SHA256=${TWIDGET_TEXT_HARFBUZZ_SHA256})
        FetchContent_Populate(twt_harfbuzz_src)
        set(TWIDGET_TEXT_HARFBUZZ "${twt_harfbuzz_src_SOURCE_DIR}")
    endif()
    if(NOT TARGET tw_harfbuzz)
        # Compiled through a one-line file in the build tree: a source outside it gets an object path that repeats
        # the source's absolute path, too long for the NDK's ninja on deep checkouts.
        set(_twt_hb_unit "${CMAKE_CURRENT_BINARY_DIR}/tw_harfbuzz.cc")
        set(_twt_hb_line "#include \"harfbuzz.cc\"\n")
        if(EXISTS "${_twt_hb_unit}")
            file(READ "${_twt_hb_unit}" _twt_hb_old)
        endif()
        if(NOT "${_twt_hb_old}" STREQUAL "${_twt_hb_line}")
            file(WRITE "${_twt_hb_unit}" "${_twt_hb_line}")
        endif()
        add_library(tw_harfbuzz STATIC "${_twt_hb_unit}")
        target_include_directories(tw_harfbuzz PUBLIC "${TWIDGET_TEXT_HARFBUZZ}/src")
        target_compile_definitions(tw_harfbuzz PRIVATE HAVE_MMAP HAVE_SYS_MMAN_H HAVE_UNISTD_H HAVE_PTHREAD)
        target_compile_options(tw_harfbuzz PRIVATE -O2 -w -fno-exceptions -fno-rtti -fvisibility=hidden)
        set_target_properties(tw_harfbuzz PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
    add_library(twt_system INTERFACE)
    if(ANDROID)
        target_link_libraries(twt_system INTERFACE tw_harfbuzz android log z dl)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(TWT_FC REQUIRED IMPORTED_TARGET fontconfig zlib)
        target_link_libraries(twt_system INTERFACE tw_harfbuzz PkgConfig::TWT_FC)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TWT_TEXT REQUIRED IMPORTED_TARGET harfbuzz fontconfig zlib)
    add_library(twt_system INTERFACE)
    target_link_libraries(twt_system INTERFACE PkgConfig::TWT_TEXT)
endif()

add_library(twidget_text STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/text_backend.cpp")
add_library(twidget::text ALIAS twidget_text)
target_include_directories(twidget_text PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${TWT_ROOT}/core/src")
target_link_libraries(twidget_text PUBLIC twt_system)
set_target_properties(twidget_text PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(twidget_text PUBLIC cxx_std_20)
target_compile_options(twidget_text PRIVATE -Wall -Wextra -Wno-unused-parameter)
