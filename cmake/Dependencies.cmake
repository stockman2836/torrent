include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL 7.62 REQUIRED)

find_package(tinyxml2 10.0.0 EXACT CONFIG QUIET COMPONENTS static)
if(NOT TARGET tinyxml2::tinyxml2)
    set(tinyxml2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(tinyxml2_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(tinyxml2
        URL https://codeload.github.com/leethomason/tinyxml2/tar.gz/refs/tags/10.0.0
        URL_HASH SHA256=3bdf15128ba16686e69bce256cc468e76c7b94ff2c7f391cc5ec09e40bff3839)
    FetchContent_MakeAvailable(tinyxml2)
endif()

find_package(spdlog CONFIG QUIET)
if(NOT TARGET spdlog::spdlog)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.12.0)
    FetchContent_MakeAvailable(spdlog)
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3)
    FetchContent_MakeAvailable(json)
endif()

if(ENABLE_UTP)
    find_path(LIBUTP_INCLUDE_DIR utp.h HINTS "${PROJECT_SOURCE_DIR}/external/libutp")
    find_library(LIBUTP_LIBRARY NAMES utp libutp HINTS "${PROJECT_SOURCE_DIR}/external/libutp")
    if(NOT LIBUTP_INCLUDE_DIR OR NOT LIBUTP_LIBRARY)
        message(FATAL_ERROR "ENABLE_UTP requires libutp. Set LIBUTP_INCLUDE_DIR and LIBUTP_LIBRARY or disable ENABLE_UTP.")
    endif()
endif()
