include(FetchContent)

function(richeditor_add_sweetline)
    if (NOT TARGET SweetLine3p::Oniguruma)
        if (NOT TARGET onig)
            set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
            set(BUILD_SHARED_LIBS OFF)
            set(BUILD_TEST OFF)
            set(INSTALL_DOCUMENTATION OFF)
            set(CMAKE_POSITION_INDEPENDENT_CODE ON)
            FetchContent_Declare(richeditor_oniguruma
                    GIT_REPOSITORY https://github.com/kkos/oniguruma.git
                    GIT_TAG 4ef89209a239c1aea328cf13c05a2807e5c146d1
            )
            FetchContent_MakeAvailable(richeditor_oniguruma)
        endif ()

        # SweetLine uses a nested include path for the Oniguruma public header.
        set(onig_include "${CMAKE_CURRENT_BINARY_DIR}/sweetline-include")
        file(CONFIGURE OUTPUT "${onig_include}/oniguruma/oniguruma.h" CONTENT "#include <oniguruma.h>\n")
        add_library(richeditor_oniguruma_headers INTERFACE)
        target_include_directories(richeditor_oniguruma_headers INTERFACE
                "${onig_include}" "$<TARGET_PROPERTY:onig,INTERFACE_INCLUDE_DIRECTORIES>")
        target_link_libraries(richeditor_oniguruma_headers INTERFACE onig)
        add_library(SweetLine3p::Oniguruma ALIAS richeditor_oniguruma_headers)
    endif ()

    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(SWEETLINE_BUILD_STATIC ON)
    set(SWEETLINE_BUILD_SHARED OFF)
    set(SWEETLINE_BUILD_TESTS OFF)
    set(SWEETLINE_BUILD_ANDROID_JNI OFF)
    set(SWEETLINE_BUILD_APPLE_FRAMEWORK OFF)
    set(SWEETLINE_BUILD_WASM_EMBIND OFF)
    FetchContent_Declare(richeditor_sweetline
            GIT_REPOSITORY https://github.com/FinalScave/SweetLine.git
            GIT_TAG d4b0d08245a6c256fff50fff883e116c8226ec96
    )
    FetchContent_MakeAvailable(richeditor_sweetline)
    if (TARGET sweetline_3p_catch2)
        set_property(TARGET sweetline_3p_catch2 PROPERTY EXCLUDE_FROM_ALL ON)
    endif ()

    # Embed the selected profiles so highlighting works without filesystem access at runtime.
    set(profiles "")
    foreach (language IN ITEMS "c|C" "cpp|C++" "csharp|C#" "css|CSS" "go|Go" "html|HTML" "java|Java"
            "javascript|JavaScript" "json|JSON" "kotlin|Kotlin" "python|Python" "rust|Rust" "shell|Shell"
            "sql|SQL" "swift|Swift" "typescript|TypeScript" "yaml|YAML")
        string(REPLACE "|" ";" language "${language}")
        list(GET language 0 id)
        list(GET language 1 label)
        set(profile "${id}")
        if (id STREQUAL "json")
            set(profile json-sweetline)
        endif ()
        set(path "${richeditor_sweetline_SOURCE_DIR}/syntaxes/${profile}.json")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
        file(READ "${path}" syntax)
        string(APPEND profiles "{{\"${id}\", \"${label}\"}, R\"sl_profile(${syntax})sl_profile\"},\n")
    endforeach ()
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sweetline-include/richeditor_syntax_profiles.inc"
            CONTENT "@profiles@" @ONLY)
endfunction()

richeditor_add_sweetline()
target_include_directories(huxerui_richeditor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/sweetline-include")
target_link_libraries(huxerui_richeditor PRIVATE SweetLine::sweetline_static)
