# Run after linking, outside project mode as required by GET_RUNTIME_DEPENDENCIES.
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${TOOLCHAIN_BIN}/objdump.exe")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${APP}"
    DIRECTORIES "${TOOLCHAIN_BIN}"
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR missing
    PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
    POST_EXCLUDE_REGEXES ".*[/\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\].*")
if(missing)
    message(FATAL_ERROR "Missing desktop runtime DLLs: ${missing}")
endif()
get_filename_component(destination "${APP}" DIRECTORY)
foreach(dependency IN LISTS dependencies)
    file(COPY "${dependency}" DESTINATION "${destination}")
endforeach()
