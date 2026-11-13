# ZIP packaging is separate from the normal CLI/desktop install under bin/.
set(PORTABLE_RUNTIME_DIRECTORIES "" CACHE STRING "Extra search directories for portable runtime DLLs")
set(PORTABLE_LICENSE_FILES "" CACHE STRING "Upstream license/notice files to include in the portable archive")
list(APPEND PORTABLE_LICENSE_FILES "${PROJECT_SOURCE_DIR}/packaging/licenses/tinyxml2/LICENSE.txt")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$" OR
   CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]64$" OR
   CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
    set(portable_arch arm64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(portable_arch x64)
else()
    set(portable_arch x86)
endif()
set(PORTABLE_PACKAGE_NAME "TorrentClient-${PROJECT_VERSION}-windows-${portable_arch}")
set(PORTABLE_TOOLCHAIN_BIN "")
if(MINGW)
    get_filename_component(PORTABLE_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
endif()

set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
set(PORTABLE_SYSTEM_RUNTIME "${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")

configure_file("${PROJECT_SOURCE_DIR}/cmake/InstallPortable.cmake.in"
    "${PROJECT_BINARY_DIR}/portable/install.in.cmake" @ONLY)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/portable/install-$<CONFIG>.cmake"
    INPUT "${PROJECT_BINARY_DIR}/portable/install.in.cmake")
install(SCRIPT "${PROJECT_BINARY_DIR}/portable/install-$<CONFIG>.cmake"
    COMPONENT Portable EXCLUDE_FROM_ALL)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME TorrentClient)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "${PORTABLE_PACKAGE_NAME}")
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}/dist")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL Portable)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_VERBATIM_VARIABLES YES)
include(CPack)
