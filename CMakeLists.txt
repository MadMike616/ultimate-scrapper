cmake_minimum_required(VERSION 3.29)

project(UltimateScrapper VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ULTIMATE_SCRAPPER_LOCAL_SDK_DIR "" CACHE PATH
    "Optional PluginSDK include tree for offline development builds")
if(ULTIMATE_SCRAPPER_LOCAL_SDK_DIR)
    add_library(D2RLPlugin INTERFACE)
    add_library(D2RLPlugin::D2RLPlugin ALIAS D2RLPlugin)
    target_include_directories(D2RLPlugin INTERFACE
        "${ULTIMATE_SCRAPPER_LOCAL_SDK_DIR}/include")
else()
    include(FetchContent)
    set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(D2RLPLUGIN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        D2RLoaderPluginSDK
        GIT_REPOSITORY https://github.com/D2RLoader/PluginSDK.git
        GIT_TAG 6eb8f8b6192868214706bd6d528c5294f2f551b7
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(D2RLoaderPluginSDK)
endif()

include(FetchContent)
if(NOT TARGET tomlplusplus::tomlplusplus)
    set(TOMLPLUSPLUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(TOMLPLUSPLUS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(tomlplusplus)
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/ultimate-scrapper.toml"
    ULTIMATE_SCRAPPER_DEFAULT_TOML)
configure_file(src/default_config.hpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/default_config.hpp"
    @ONLY NEWLINE_STYLE LF)

add_library(ultimate_scrapper SHARED
    src/plugin.cpp
    src/plugin.rc
)
target_link_libraries(ultimate_scrapper PRIVATE D2RLPlugin::D2RLPlugin)
target_link_libraries(ultimate_scrapper PRIVATE tomlplusplus::tomlplusplus)
target_include_directories(ultimate_scrapper PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(ultimate_scrapper PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
)
set_target_properties(ultimate_scrapper PROPERTIES
    OUTPUT_NAME "d2rl-ultimate-scrapper"
    PREFIX ""
)

if(MSVC)
    target_compile_options(ultimate_scrapper PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(ultimate_scrapper PRIVATE /INCREMENTAL:NO /Brepro)
endif()

