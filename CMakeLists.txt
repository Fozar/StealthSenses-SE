cmake_minimum_required(VERSION 3.28)
project(StealthSenses VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# We build a DLL plugin, not an installable package — skip all install() rules
# (prevents CommonLibSSE's install(EXPORT) from failing on FetchContent targets)
set(CMAKE_SKIP_INSTALL_RULES TRUE)

# fmt — OVERRIDE_FIND_PACKAGE intercepts CommonLibSSE's find_package(fmt)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 12.1.0
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(fmt)

# spdlog — built against the fmt above
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.16.0
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

# rapidcsv — single header only; CommonLibSSE's find_path needs it
FetchContent_Declare(
    rapidcsv
    GIT_REPOSITORY https://github.com/d99kris/rapidcsv.git
    GIT_TAG v8.83
    GIT_SHALLOW TRUE
    DOWNLOAD_ONLY YES
)
FetchContent_MakeAvailable(rapidcsv)
set(RAPIDCSV_INCLUDE_DIRS "${rapidcsv_SOURCE_DIR}/src" CACHE PATH "" FORCE)

# nlohmann/json — header-only JSON parser for the config file
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# DirectXTK — CommonLibSSE-NG 10.x only includes its SimpleMath.h header, so expose the
# headers as an INTERFACE target instead of building the library. SOURCE_SUBDIR points
# nowhere so DirectXTK's own CMake never runs.
FetchContent_Declare(
    directxtk
    GIT_REPOSITORY https://github.com/microsoft/DirectXTK.git
    GIT_TAG oct2025
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _headers_only
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(directxtk)
add_library(DirectXTKHeaders INTERFACE)
target_include_directories(DirectXTKHeaders INTERFACE "${directxtk_SOURCE_DIR}/Inc")
add_library(Microsoft::DirectXTK ALIAS DirectXTKHeaders)

# CommonLibSSE-NG — maintained fork (alandtse/CommonLibVR, "ng" line). Same tag as
# DragonbornPresence-SE: known to build with this toolchain and to handle AE 1.7.x.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/alandtse/CommonLibVR.git
    GIT_TAG v10.1.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(CommonLibSSE)
include("${commonlibsse_SOURCE_DIR}/cmake/CommonLibSSE.cmake")

add_commonlibsse_plugin(StealthSenses
    SOURCES
        main.cpp
        Config.cpp
        Trail.cpp
        Trackers.cpp
        Serialization.cpp
        Telemetry.cpp
    NAME "StealthSenses"
    VERSION "${PROJECT_VERSION}"
    USE_ADDRESS_LIBRARY
)

target_include_directories(StealthSenses PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# <Windows.h> is included alongside RE headers; its min/max macros break them
target_compile_definitions(StealthSenses PRIVATE NOMINMAX)

target_precompile_headers(StealthSenses PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pch.h")

target_link_libraries(StealthSenses PRIVATE nlohmann_json::nlohmann_json)

set(_pkg_plugins "${CMAKE_CURRENT_BINARY_DIR}/_package/SKSE/Plugins")
file(MAKE_DIRECTORY "${_pkg_plugins}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/StealthSensesConfig.json" DESTINATION "${_pkg_plugins}")

add_custom_command(TARGET StealthSenses POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:StealthSenses>"
        "${_pkg_plugins}/StealthSenses.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/StealthSensesConfig.json"
        "${_pkg_plugins}/StealthSensesConfig.json"
    COMMAND ${CMAKE_COMMAND} -E tar cf
        "${CMAKE_CURRENT_BINARY_DIR}/StealthSenses.zip" --format=zip
        -- "SKSE/Plugins/StealthSenses.dll"
           "SKSE/Plugins/StealthSensesConfig.json"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/_package"
    COMMENT "Packaging StealthSenses.zip"
)

# Dev deploy: copy the DLL into a mod manager folder after each build,
# e.g. -DSTEALTHSENSES_DEPLOY_DIR=D:/Modding/MO2/mods/StealthSenses/SKSE/Plugins.
# The config is not copied: the deployed one holds test settings.
set(STEALTHSENSES_DEPLOY_DIR "" CACHE PATH "Copy the built DLL here after each build (empty = off)")
if(STEALTHSENSES_DEPLOY_DIR)
    add_custom_command(TARGET StealthSenses POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${STEALTHSENSES_DEPLOY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:StealthSenses>" "${STEALTHSENSES_DEPLOY_DIR}"
        COMMENT "Deploying StealthSenses.dll to ${STEALTHSENSES_DEPLOY_DIR}"
    )
endif()
