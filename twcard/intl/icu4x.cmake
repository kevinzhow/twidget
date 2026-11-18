# SPEC S8.2 and PLAN D3: one static ICU4X provider and compiled locale data per process.
find_program(TWCARD_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)
get_filename_component(_tw_cargo_bin "${TWCARD_CARGO}" DIRECTORY)
set(_tw_cargo_target_dir "${CMAKE_CURRENT_BINARY_DIR}/icu4x-target")
set(_tw_cargo_args)
set(_tw_cargo_arch "")
if(ANDROID)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(_tw_cargo_arch aarch64-linux-android)
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(_tw_cargo_arch x86_64-linux-android)
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(_tw_cargo_arch armv7-linux-androideabi)
    elseif(ANDROID_ABI STREQUAL "x86")
        set(_tw_cargo_arch i686-linux-android)
    else()
        message(FATAL_ERROR "twcard ICU4X has no Rust target for Android ABI ${ANDROID_ABI}")
    endif()
elseif(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "twcard ICU4X requires a Rust target for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(_tw_cargo_output "${_tw_cargo_target_dir}")
if(_tw_cargo_arch)
    list(APPEND _tw_cargo_args --target "${_tw_cargo_arch}")
    string(APPEND _tw_cargo_output "/${_tw_cargo_arch}")
endif()
set(_tw_icu_archive "${_tw_cargo_output}/release/libtwcard_icu4x.a")
add_custom_command(
    OUTPUT "${_tw_icu_archive}"
    COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=${_tw_cargo_bin}:$ENV{PATH}"
        "CARGO_HOME=${CMAKE_CURRENT_BINARY_DIR}/cargo-home"
        "CARGO_TARGET_DIR=${_tw_cargo_target_dir}"
        "CARGO_NET_GIT_FETCH_WITH_CLI=true"
        "${TWCARD_CARGO}" build --manifest-path "${CMAKE_CURRENT_LIST_DIR}/Cargo.toml"
        --locked --release --jobs 1 ${_tw_cargo_args}
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/Cargo.toml" "${CMAKE_CURRENT_LIST_DIR}/Cargo.lock" "${CMAKE_CURRENT_LIST_DIR}/src/lib.rs"
    VERBATIM)
add_custom_target(twcard_icu4x_archive DEPENDS "${_tw_icu_archive}")
add_library(twcard_icu4x STATIC IMPORTED GLOBAL)
set_target_properties(twcard_icu4x PROPERTIES IMPORTED_LOCATION "${_tw_icu_archive}")
add_dependencies(twcard_icu4x twcard_icu4x_archive)
