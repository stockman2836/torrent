include(FetchContent)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

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
