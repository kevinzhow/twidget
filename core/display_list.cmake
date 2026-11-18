# twidget_display_list (twidget::display_list): the part of TwidgetCore that needs no card runtime, for
# TwidgetCore and other hosts (a document reader) alike: the display list's binary encoding (list_encoding.cpp, the
# frame format's display lists) and the image header reader (png.cpp). Included by twidget's top-level CMakeLists.txt
# and by hosts that build on twidget without the core.
if(NOT TARGET twidget_display_list)
    add_library(twidget_display_list STATIC
        "${CMAKE_CURRENT_LIST_DIR}/src/list_encoding.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/src/png.cpp")
    add_library(twidget::display_list ALIAS twidget_display_list)
    target_include_directories(twidget_display_list PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
    set_target_properties(twidget_display_list PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(twidget_display_list PRIVATE /utf-8 /W3)
    else()
        target_compile_options(twidget_display_list PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endif()
