cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(lib_rich_editor VERSION 0.1.0 LANGUAGES NONE)

option(HUXERUI_RICHEDITOR_BUILD_TESTS "Build Lib-RichEditor tests" OFF)
include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

file(GLOB_RECURSE LIBRARY_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
if (ANDROID)
    file(GLOB_RECURSE LIBRARY_ANDROID_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/src/main/cpp/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_ANDROID_SOURCE_FILES})
endif ()
if (WIN32)
    file(GLOB_RECURSE LIBRARY_WINDOWS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WINDOWS_SOURCE_FILES})
endif ()
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB_RECURSE LIBRARY_LINUX_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_LINUX_SOURCE_FILES})
endif ()
if (APPLE AND NOT IOS)
    file(GLOB_RECURSE LIBRARY_MACOS_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.cxx"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src/*.mm"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_MACOS_SOURCE_FILES})
endif ()
if (EMSCRIPTEN)
    file(GLOB_RECURSE LIBRARY_WEB_SOURCE_FILES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cc"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/src/*.cxx"
    )
    list(APPEND LIBRARY_SOURCE_FILES ${LIBRARY_WEB_SOURCE_FILES})
endif ()

huxerui_add_library(huxerui_richeditor
        SOURCES
            ${LIBRARY_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            huxerui_richeditor
)

add_library(HuxerUI::RichEditor ALIAS huxerui_richeditor)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(huxerui_richeditor PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)

include(FetchContent)
if (NOT TARGET HuxerUI::Markdown)
    function(richeditor_add_markdown_parser)
        set(HUXERUI_MARKDOWN_BUILD_VIEW OFF)
        FetchContent_Declare(richeditor_markdown
                GIT_REPOSITORY https://github.com/HuxerUI/Lib-Markdown.git
                GIT_TAG 247ed949f571c186aeffb1d57db4c4b7407538e0
        )
        FetchContent_MakeAvailable(richeditor_markdown)
    endfunction()
    richeditor_add_markdown_parser()
endif ()
if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif ()
if (NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(nlohmann_json
            URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
            URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif ()
target_link_libraries(huxerui_richeditor PRIVATE HuxerUI::Markdown nlohmann_json::nlohmann_json)
include(cmake/SweetLine.cmake)
if (TARGET HuxerUI::huxerui_static)
    target_link_libraries(huxerui_richeditor PUBLIC HuxerUI::huxerui_static)
else ()
    target_link_libraries(huxerui_richeditor PUBLIC HuxerUI::huxerui)
endif ()
target_compile_features(huxerui_richeditor PUBLIC cxx_std_20)

if (HUXERUI_RICHEDITOR_BUILD_TESTS)
    if (NOT TARGET HuxerUI::testing AND HuxerUI_DIR)
        find_package(HuxerUI CONFIG REQUIRED OPTIONAL_COMPONENTS testing)
    endif ()
    enable_testing()
    add_subdirectory(tests)
endif ()
