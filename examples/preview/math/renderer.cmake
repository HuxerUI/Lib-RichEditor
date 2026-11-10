option(FOLIO_BUILD_MATH_RENDERER "Build Folio's optional LaTeX renderer" ON)
if(NOT FOLIO_BUILD_MATH_RENDERER)
    return()
endif()

find_program(FOLIO_CARGO cargo)
find_program(FOLIO_RUSTC rustc)

if(ANDROID)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(folio_rust_target aarch64-linux-android)
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(folio_rust_target armv7-linux-androideabi)
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(folio_rust_target x86_64-linux-android)
    elseif(ANDROID_ABI STREQUAL "x86")
        set(folio_rust_target i686-linux-android)
    endif()
elseif(APPLE AND NOT IOS)
    set(folio_arch "${CMAKE_OSX_ARCHITECTURES}")
    if(NOT folio_arch)
        set(folio_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(folio_arch STREQUAL "arm64")
        set(folio_rust_target aarch64-apple-darwin)
    elseif(folio_arch STREQUAL "x86_64")
        set(folio_rust_target x86_64-apple-darwin)
    endif()
endif()

if(NOT FOLIO_CARGO OR NOT FOLIO_RUSTC OR NOT folio_rust_target)
    return()
endif()

execute_process(COMMAND "${FOLIO_RUSTC}" --print target-libdir --target "${folio_rust_target}"
                OUTPUT_VARIABLE folio_rust_libdir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
file(GLOB folio_rust_std "${folio_rust_libdir}/libstd-*.rlib")
if(NOT folio_rust_std)
    message(STATUS "Folio formulas use a text fallback. Install Rust target ${folio_rust_target} to typeset them.")
    return()
endif()

set(folio_math_dir "${CMAKE_CURRENT_LIST_DIR}")
set(folio_math_build "${CMAKE_CURRENT_BINARY_DIR}/folio-math")
set(folio_math_archive "${folio_math_build}/${folio_rust_target}/release/libfolio_math.a")
add_custom_command(OUTPUT "${folio_math_archive}"
        COMMAND "${FOLIO_CARGO}" build --locked --release --target "${folio_rust_target}"
                --manifest-path "${folio_math_dir}/Cargo.toml" --target-dir "${folio_math_build}"
        DEPENDS "${folio_math_dir}/Cargo.toml" "${folio_math_dir}/Cargo.lock" "${folio_math_dir}/src/lib.rs"
        VERBATIM)
add_custom_target(folio_math_build DEPENDS "${folio_math_archive}")
add_dependencies(example_lib_rich_editor folio_math_build)
target_link_libraries(example_lib_rich_editor PRIVATE "${folio_math_archive}")
target_compile_definitions(example_lib_rich_editor PRIVATE FOLIO_MATH_RENDERER=1)
