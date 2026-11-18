# twcard: the card runtime's trusted host side (SPEC S0): Engine (S1), Validator (S9), CardHost (S10) and the Intl
# providers (S8.2), shared by twidget_flutter (Linux, Android) and the Apple package (PLAN P14).
#
# QuickJS: the TelegramMessenger/quickjs fork (MIT, PLAN D2), fetched at a pinned commit with its SHA-256 checked;
# TWIDGET_QUICKJS_DIR (a checkout or an unpacked archive of that commit) builds that instead (offline builds).
# Pinned commit: b526a817 (master, 2026-11-10: "Merge tdesktop-fixes into master"), the 2026-11 security fixes and
# the uncatchable interrupt / out-of-memory state (JS_IsUncatchableError) that SPEC S1.4 needs.
if(NOT TARGET twcard)
    include("${CMAKE_CURRENT_LIST_DIR}/../base/base.cmake")

    set(TWIDGET_QUICKJS_COMMIT "b526a817f188ef861eea7666d68361c7851673a0" CACHE STRING "TelegramMessenger/quickjs commit")
    set(TWIDGET_QUICKJS_SHA256 "0abcea272b0066b18ca33d1a768654018a44b6b0412c86698764c2ad70baff09" CACHE STRING "SHA-256 of that commit's archive")
    set(TWIDGET_QUICKJS_DIR "" CACHE PATH "a QuickJS source tree to build instead of the pinned commit")
    if(NOT TWIDGET_QUICKJS_DIR AND DEFINED ENV{TWIDGET_QUICKJS_DIR})
        set(TWIDGET_QUICKJS_DIR "$ENV{TWIDGET_QUICKJS_DIR}")
    endif()
    if(TWIDGET_QUICKJS_DIR)
        set(TW_QUICKJS_SRC "${TWIDGET_QUICKJS_DIR}")
    else()
        include(FetchContent)
        FetchContent_Declare(tw_quickjs_src
            URL https://github.com/TelegramMessenger/quickjs/archive/${TWIDGET_QUICKJS_COMMIT}.tar.gz
            URL_HASH SHA256=${TWIDGET_QUICKJS_SHA256})
        FetchContent_GetProperties(tw_quickjs_src)
        if(NOT tw_quickjs_src_POPULATED)
            FetchContent_Populate(tw_quickjs_src)
        endif()
        set(TW_QUICKJS_SRC "${tw_quickjs_src_SOURCE_DIR}")
    endif()
    if(NOT EXISTS "${TW_QUICKJS_SRC}/quickjs.c")
        message(FATAL_ERROR "twcard: no QuickJS sources in ${TW_QUICKJS_SRC}")
    endif()

    # The engine alone: no quickjs-libc (no files, no OS access for card code, SPEC I2).
    add_library(twcard_quickjs STATIC
        "${TW_QUICKJS_SRC}/dtoa.c" "${TW_QUICKJS_SRC}/libregexp.c" "${TW_QUICKJS_SRC}/libunicode.c" "${TW_QUICKJS_SRC}/quickjs.c")
    target_include_directories(twcard_quickjs SYSTEM PUBLIC "${TW_QUICKJS_SRC}")
    target_compile_definitions(twcard_quickjs PRIVATE _GNU_SOURCE QUICKJS_NG_BUILD)
    set_target_properties(twcard_quickjs PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
    find_package(Threads REQUIRED)
    target_link_libraries(twcard_quickjs PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    if(NOT WIN32)
        target_link_libraries(twcard_quickjs PUBLIC m)
    endif()
    if(MSVC)
        target_compile_options(twcard_quickjs PRIVATE /w)
    else()
        target_compile_options(twcard_quickjs PRIVATE -w -funsigned-char)
    endif()

    add_library(twcard STATIC
        "${CMAKE_CURRENT_LIST_DIR}/src/throttle.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/engine.cpp")
    add_library(twidget::twcard ALIAS twcard)
    target_include_directories(twcard PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include" PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(twcard PUBLIC twidget::base PRIVATE twcard_quickjs)
    target_compile_features(twcard PUBLIC cxx_std_20)
    set_target_properties(twcard PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(twcard PRIVATE /utf-8 /W3)
    else()
        target_compile_options(twcard PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()

    # The build-time bytecode compiler for the runtime bundle (not for cross builds: it runs on the build machine).
    if(NOT CMAKE_CROSSCOMPILING)
        add_executable(twcard_bytecode "${CMAKE_CURRENT_LIST_DIR}/tools/twcard_bytecode.cpp")
        target_link_libraries(twcard_bytecode PRIVATE twcard_quickjs)
    endif()

    if(TWIDGET_BUILD_TESTS)
        foreach(_t throttle engine)
            add_executable(twcard_${_t}_tests "${CMAKE_CURRENT_LIST_DIR}/tests/${_t}_tests.cpp")
            target_link_libraries(twcard_${_t}_tests PRIVATE twcard twcard_quickjs)
            target_include_directories(twcard_${_t}_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
            add_test(NAME twcard_${_t} COMMAND twcard_${_t}_tests)
        endforeach()
        target_compile_definitions(twcard_engine_tests PRIVATE TWCARD_TEST_BUNDLES="${CMAKE_CURRENT_LIST_DIR}/tests/bundles")
        if(TARGET twcard_bytecode)
            add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/twcard-test-runtime.qbc"
                COMMAND twcard_bytecode "${CMAKE_CURRENT_LIST_DIR}/tests/bundles/test-runtime.js" "${CMAKE_CURRENT_BINARY_DIR}/twcard-test-runtime.qbc"
                DEPENDS twcard_bytecode "${CMAKE_CURRENT_LIST_DIR}/tests/bundles/test-runtime.js")
            add_custom_target(twcard_test_bytecode DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/twcard-test-runtime.qbc")
            add_dependencies(twcard_engine_tests twcard_test_bytecode)
            target_compile_definitions(twcard_engine_tests PRIVATE TWCARD_TEST_BYTECODE="${CMAKE_CURRENT_BINARY_DIR}/twcard-test-runtime.qbc")
        endif()
    endif()
endif()
