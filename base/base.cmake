# twidget_base (twidget::base): what TwidgetCore and the card host (twcard) share and nothing else provides:
#   twidget/json.h            twidget::Json over yyjson (SPEC S11.1)
#   twidget/gesture_policy.h  the gesture claim and the arbiter (SPEC S11.2)
#   twidget/paint_limits.h    the heavy-element limits and their codes (SPEC S10.4, S11.3)
# Included by twidget's top-level CMakeLists.txt and by hosts that build on twidget.
#
# yyjson (MIT): fetched at a pinned release, SHA-256 checked; TWIDGET_YYJSON_DIR (a checkout or an unpacked release)
# builds that instead (offline builds).
if(NOT TARGET twidget_base)
    set(TWIDGET_YYJSON_VERSION "0.13.0" CACHE STRING "yyjson release fetched when TWIDGET_YYJSON_DIR is not given")
    set(TWIDGET_YYJSON_SHA256 "34e0f62a2bc11ab20d601e8ca1cc2b2079503aa45119a19133d89d19b94a0fae" CACHE STRING "SHA-256 of that release's source tarball")
    set(TWIDGET_YYJSON_DIR "" CACHE PATH "a yyjson source tree to build instead of the pinned release")
    if(NOT TWIDGET_YYJSON_DIR AND DEFINED ENV{TWIDGET_YYJSON_DIR})
        set(TWIDGET_YYJSON_DIR "$ENV{TWIDGET_YYJSON_DIR}")
    endif()
    if(TWIDGET_YYJSON_DIR)
        set(TW_YYJSON_SRC "${TWIDGET_YYJSON_DIR}")
    else()
        include(FetchContent)
        FetchContent_Declare(tw_yyjson_src
            URL https://github.com/ibireme/yyjson/archive/refs/tags/${TWIDGET_YYJSON_VERSION}.tar.gz
            URL_HASH SHA256=${TWIDGET_YYJSON_SHA256})
        FetchContent_GetProperties(tw_yyjson_src)
        if(NOT tw_yyjson_src_POPULATED)
            FetchContent_Populate(tw_yyjson_src)
        endif()
        set(TW_YYJSON_SRC "${tw_yyjson_src_SOURCE_DIR}")
    endif()
    if(NOT EXISTS "${TW_YYJSON_SRC}/src/yyjson.c")
        message(FATAL_ERROR "twidget: no yyjson sources in ${TW_YYJSON_SRC}")
    endif()

    add_library(twidget_yyjson STATIC "${TW_YYJSON_SRC}/src/yyjson.c")
    target_include_directories(twidget_yyjson PUBLIC "${TW_YYJSON_SRC}/src")
    set_target_properties(twidget_yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

    add_library(twidget_base STATIC
        "${CMAKE_CURRENT_LIST_DIR}/src/json.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/gesture_policy.cpp")
    add_library(twidget::base ALIAS twidget_base)
    target_include_directories(twidget_base PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(twidget_base PRIVATE twidget_yyjson)
    target_compile_features(twidget_base PUBLIC cxx_std_20)
    set_target_properties(twidget_base PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(twidget_base PRIVATE /utf-8 /W3)
    else()
        target_compile_options(twidget_base PRIVATE -Wall -Wextra)
    endif()
endif()
