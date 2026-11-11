option(FOLIO_BUILD_MATH_RENDERER "Build ${DRAFTSPAN_APP_NAME}'s LaTeX renderer" ON)
set(folio_math_build "${CMAKE_CURRENT_BINARY_DIR}/folio-math")
set(folio_ios_math_link_file "${folio_math_build}/ios-link.rsp")
if(NOT FOLIO_BUILD_MATH_RENDERER)
    if(IOS)
        file(GENERATE OUTPUT "${folio_ios_math_link_file}" CONTENT "")
    endif()
    return()
endif()

set(folio_rust_targets)
set(folio_math_archive_name libfolio_math.a)
if(EMSCRIPTEN)
    set(folio_rust_targets wasm32-unknown-emscripten)
elseif(ANDROID)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(folio_rust_targets aarch64-linux-android)
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(folio_rust_targets armv7-linux-androideabi)
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(folio_rust_targets x86_64-linux-android)
    elseif(ANDROID_ABI STREQUAL "x86")
        set(folio_rust_targets i686-linux-android)
    endif()
elseif(APPLE)
    set(folio_architectures "${CMAKE_OSX_ARCHITECTURES}")
    string(TOLOWER "${CMAKE_OSX_SYSROOT}" folio_apple_sdk)
    if(NOT IOS)
        set(folio_apple_arm64 aarch64-apple-darwin)
        set(folio_apple_x86_64 x86_64-apple-darwin)
    elseif(folio_apple_sdk MATCHES "iphonesimulator")
        set(folio_apple_arm64 aarch64-apple-ios-sim)
        set(folio_apple_x86_64 x86_64-apple-ios)
    else()
        set(folio_apple_arm64 aarch64-apple-ios)
        if(NOT folio_architectures)
            set(folio_architectures arm64)
        endif()
    endif()
    if(NOT folio_architectures)
        set(folio_architectures "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    foreach(folio_arch IN LISTS folio_architectures)
        set(folio_rust_target "${folio_apple_${folio_arch}}")
        if(NOT folio_rust_target)
            message(FATAL_ERROR "${DRAFTSPAN_APP_NAME} has no Rust target for ${CMAKE_SYSTEM_NAME} ${folio_apple_sdk} ${folio_arch}. "
                                "Set FOLIO_BUILD_MATH_RENDERER=OFF to display formula source.")
        endif()
        list(APPEND folio_rust_targets "${folio_rust_target}")
    endforeach()
elseif(WIN32 AND MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    set(folio_rust_targets x86_64-pc-windows-msvc)
    set(folio_math_archive_name folio_math.lib)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" folio_arch)
    if(folio_arch MATCHES "^(aarch64|arm64)$")
        set(folio_rust_targets aarch64-unknown-linux-gnu)
    elseif(folio_arch MATCHES "^(x86_64|amd64)$")
        set(folio_rust_targets x86_64-unknown-linux-gnu)
    endif()
endif()

if(NOT folio_rust_targets)
    message(FATAL_ERROR "${DRAFTSPAN_APP_NAME} has no Rust target for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR} ${ANDROID_ABI}. "
                        "Set FOLIO_BUILD_MATH_RENDERER=OFF to display formula source.")
endif()

find_program(FOLIO_CARGO cargo HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin")
find_program(FOLIO_RUSTC rustc HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin")
if(NOT FOLIO_CARGO OR NOT FOLIO_RUSTC)
    message(FATAL_ERROR "${DRAFTSPAN_APP_NAME} math rendering requires Cargo and rustc. Install Rust with rustup "
                        "or set FOLIO_BUILD_MATH_RENDERER=OFF to display formula source.")
endif()

set(folio_math_dir "${CMAKE_CURRENT_LIST_DIR}")
set(folio_cargo_toolchain)
set(folio_cargo_build_options)
set(folio_cargo_environment "RUSTC=${FOLIO_RUSTC}")
if(EMSCRIPTEN)
    # Build the Rust standard library with the Emscripten toolchain used by the app.
    set(folio_cargo_toolchain +nightly)
    set(folio_cargo_build_options -Z build-std=std,panic_abort)
    execute_process(COMMAND "${FOLIO_RUSTC}" +nightly --print sysroot
                    WORKING_DIRECTORY "${folio_math_dir}"
                    RESULT_VARIABLE folio_rust_result OUTPUT_VARIABLE folio_rust_sysroot
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT folio_rust_result STREQUAL "0"
            OR NOT EXISTS "${folio_rust_sysroot}/lib/rustlib/src/rust/library/Cargo.toml")
        message(FATAL_ERROR "${DRAFTSPAN_APP_NAME} Web math rendering requires nightly Rust and rust-src. "
                            "Run 'rustup toolchain install nightly --profile minimal --component rust-src' "
                            "or set FOLIO_BUILD_MATH_RENDERER=OFF to display formula source.")
    endif()
    # HuxerUI Web uses Emscripten's JavaScript exception ABI.
    list(APPEND folio_cargo_environment "CARGO_PROFILE_RELEASE_PANIC=abort")
    if(CMAKE_C_COMPILER)
        list(APPEND folio_cargo_environment
                "CARGO_TARGET_WASM32_UNKNOWN_EMSCRIPTEN_LINKER=${CMAKE_C_COMPILER}"
                "CC_wasm32_unknown_emscripten=${CMAKE_C_COMPILER}")
    endif()
elseif(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
    if(IOS)
        list(APPEND folio_cargo_environment "IPHONEOS_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        list(APPEND folio_cargo_environment "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
endif()

list(REMOVE_DUPLICATES folio_rust_targets)
set(folio_math_archives)
foreach(folio_rust_target IN LISTS folio_rust_targets)
    if(NOT EMSCRIPTEN)
        execute_process(COMMAND "${FOLIO_RUSTC}" --print target-libdir --target "${folio_rust_target}"
                        WORKING_DIRECTORY "${folio_math_dir}"
                        RESULT_VARIABLE folio_rust_result OUTPUT_VARIABLE folio_rust_libdir
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        file(GLOB folio_rust_std "${folio_rust_libdir}/libstd-*.rlib")
        if(NOT folio_rust_result STREQUAL "0" OR NOT folio_rust_std)
            message(FATAL_ERROR "${DRAFTSPAN_APP_NAME} math rendering requires Rust target ${folio_rust_target}. "
                                "Run 'rustup target add ${folio_rust_target}' "
                                "or set FOLIO_BUILD_MATH_RENDERER=OFF to display formula source.")
        endif()
    endif()

    set(folio_slice "${folio_math_build}/${folio_rust_target}/release/${folio_math_archive_name}")
    add_custom_command(OUTPUT "${folio_slice}"
            COMMAND "${CMAKE_COMMAND}" -E env ${folio_cargo_environment}
                    "${FOLIO_CARGO}" ${folio_cargo_toolchain} build ${folio_cargo_build_options}
                    --locked --release --target "${folio_rust_target}"
                    --manifest-path "${folio_math_dir}/Cargo.toml" --target-dir "${folio_math_build}"
            DEPENDS "${folio_math_dir}/Cargo.toml" "${folio_math_dir}/Cargo.lock" "${folio_math_dir}/src/lib.rs"
            WORKING_DIRECTORY "${folio_math_dir}"
            COMMENT "Building ${DRAFTSPAN_APP_NAME} math renderer for ${folio_rust_target}"
            VERBATIM)
    list(APPEND folio_math_archives "${folio_slice}")
endforeach()

list(LENGTH folio_math_archives folio_math_arch_count)
if(folio_math_arch_count GREATER 1)
    find_program(FOLIO_LIPO lipo REQUIRED)
    set(folio_math_archive "${folio_math_build}/${folio_math_archive_name}")
    add_custom_command(OUTPUT "${folio_math_archive}"
            COMMAND "${FOLIO_LIPO}" -create ${folio_math_archives} -output "${folio_math_archive}"
            DEPENDS ${folio_math_archives}
            COMMENT "Combining ${DRAFTSPAN_APP_NAME} math renderer architectures"
            VERBATIM)
else()
    list(GET folio_math_archives 0 folio_math_archive)
endif()

add_custom_target(folio_math_build DEPENDS "${folio_math_archive}")
add_dependencies(example_lib_rich_editor folio_math_build)
if(IOS)
    # Xcode links the app after CMake builds its static core.
    file(GENERATE OUTPUT "${folio_ios_math_link_file}" CONTENT "\"${folio_math_archive}\"\n")
else()
    target_link_libraries(example_lib_rich_editor PRIVATE "${folio_math_archive}")
endif()
if(WIN32)
    target_link_libraries(example_lib_rich_editor PRIVATE ntdll userenv ws2_32 dbghelp)
endif()
target_compile_definitions(example_lib_rich_editor PRIVATE FOLIO_MATH_RENDERER=1)
