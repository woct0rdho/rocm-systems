#
#
#

set(_ROCPD_SCHEMA_KEYS
    rocpd_metadata
    rocpd_tables
    rocpd_views
    rocpd_indexes
    rocpd_data_views
    rocpd_summary_views)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/versions.yml" _ROCPD_VERSION_LINES)
set(_ROCPD_SCHEMA_VERSIONS)
foreach(_LINE IN LISTS _ROCPD_VERSION_LINES)
    if(_LINE MATCHES "^    - version: \"([0-9]+\\.[0-9]+\\.[0-9]+)\"$")
        list(APPEND _ROCPD_SCHEMA_VERSIONS "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT _ROCPD_SCHEMA_VERSIONS)
    message(FATAL_ERROR "versions.yml does not declare any ROCpd schema versions")
endif()
list(SORT _ROCPD_SCHEMA_VERSIONS COMPARE NATURAL ORDER DESCENDING)
list(GET _ROCPD_SCHEMA_VERSIONS 0 ROCPD_SCHEMA_VERSION)

set(_ROCPD_CURRENT_VERSION)
foreach(_LINE IN LISTS _ROCPD_VERSION_LINES)
    if(_LINE MATCHES "^    - version: \"([0-9]+\\.[0-9]+\\.[0-9]+)\"$")
        set(_ROCPD_CURRENT_VERSION "${CMAKE_MATCH_1}")
    elseif(_ROCPD_CURRENT_VERSION STREQUAL ROCPD_SCHEMA_VERSION)
        foreach(_KEY IN LISTS _ROCPD_SCHEMA_KEYS)
            if(_LINE MATCHES "^      ${_KEY}: +([^ ]+)$")
                if(DEFINED ROCPD_SCHEMA_${_KEY})
                    message(
                        FATAL_ERROR
                            "versions.yml repeats ${_KEY} for ROCpd schema ${ROCPD_SCHEMA_VERSION}"
                        )
                endif()
                set(ROCPD_SCHEMA_${_KEY} "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endif()
endforeach()

set(rocpd_schemas versions.yml)
foreach(_KEY IN LISTS _ROCPD_SCHEMA_KEYS)
    if(NOT DEFINED ROCPD_SCHEMA_${_KEY})
        message(
            FATAL_ERROR
                "versions.yml does not define ${_KEY} for latest ROCpd schema ${ROCPD_SCHEMA_VERSION}"
            )
    endif()
    set(_SCHEMA_FILE "${ROCPD_SCHEMA_${_KEY}}")
    if(IS_ABSOLUTE "${_SCHEMA_FILE}" OR _SCHEMA_FILE MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "ROCpd schema asset escapes its schema directory: ${_SCHEMA_FILE}")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_SCHEMA_FILE}")
        message(FATAL_ERROR "ROCpd schema asset does not exist: ${_SCHEMA_FILE}")
    endif()
    list(APPEND rocpd_schemas "${_SCHEMA_FILE}")
endforeach()
list(REMOVE_DUPLICATES rocpd_schemas)

if(NOT ROCPD_SCHEMA_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "invalid latest ROCpd schema version: ${ROCPD_SCHEMA_VERSION}")
endif()
set(ROCPD_SCHEMA_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(ROCPD_SCHEMA_VERSION_MINOR "${CMAKE_MATCH_2}")
set(ROCPD_SCHEMA_VERSION_PATCH "${CMAKE_MATCH_3}")
if(ROCPD_SCHEMA_VERSION_MINOR GREATER 99 OR ROCPD_SCHEMA_VERSION_PATCH GREATER 99)
    message(
        FATAL_ERROR
            "ROCpd schema minor and patch versions must fit the SQLite user_version encoding"
        )
endif()
math(
    EXPR
    ROCPD_SCHEMA_USER_VERSION
    "${ROCPD_SCHEMA_VERSION_MAJOR} * 10000 + ${ROCPD_SCHEMA_VERSION_MINOR} * 100 + ${ROCPD_SCHEMA_VERSION_PATCH}"
    )

set(_ROCPD_LATEST_SCHEMA_DEST
    "${PROJECT_BINARY_DIR}/share/rocprofiler-sdk-rocpd/latest-schema.json")
configure_file(latest-schema.json.in "${_ROCPD_LATEST_SCHEMA_DEST}" @ONLY)

function(rocpd_configure_schema_files)
    cmake_parse_arguments(_SCHEMA "" "VERSION" "FILES" ${ARGN})
    set(_SCHEMA_COMPONENT rocpd)
    if(WIN32 AND ROCPROFILER_BUILD_WINDOWS_MINIMAL)
        set(_SCHEMA_COMPONENT tools)
    endif()
    cmake_path(GET CMAKE_CURRENT_SOURCE_DIR FILENAME _SCHEMA_DIRNAME)
    if(_SCHEMA_VERSION)
        if(NOT _SCHEMA_DIRNAME STREQUAL _SCHEMA_VERSION)
            message(
                FATAL_ERROR
                    "rocpd_configure_schema_files for version ${_SCHEMA_VERSION} not located in subdirectory matching schema version (found: ${_SCHEMA_DIRNAME})"
                )
        endif()

        set(_BINARY_DEST
            "${PROJECT_BINARY_DIR}/share/rocprofiler-sdk-rocpd/versions/${_SCHEMA_VERSION}"
            )
        set(_INSTALL_DEST "share/rocprofiler-sdk-rocpd/versions/${_SCHEMA_VERSION}")
    else()
        if(NOT _SCHEMA_DIRNAME STREQUAL "rocprofiler-sdk-rocpd")
            message(
                FATAL_ERROR
                    "rocpd_configure_schema_files for latest version not located in 'rocprofiler-sdk-rocpd' subdirectory"
                )
        endif()

        set(_BINARY_DEST "${PROJECT_BINARY_DIR}/share/rocprofiler-sdk-rocpd")
        set(_INSTALL_DEST "share/rocprofiler-sdk-rocpd")
    endif()
    foreach(_FILE ${_SCHEMA_FILES})
        configure_file(${_FILE} ${_BINARY_DEST}/${_FILE} COPYONLY)
        install(
            FILES ${_BINARY_DEST}/${_FILE}
            DESTINATION ${_INSTALL_DEST}
            COMPONENT ${_SCHEMA_COMPONENT})
    endforeach()
endfunction()

rocpd_configure_schema_files(FILES ${rocpd_schemas})
set(_SCHEMA_COMPONENT rocpd)
if(WIN32 AND ROCPROFILER_BUILD_WINDOWS_MINIMAL)
    set(_SCHEMA_COMPONENT tools)
endif()
install(
    FILES "${_ROCPD_LATEST_SCHEMA_DEST}"
    DESTINATION share/rocprofiler-sdk-rocpd
    COMPONENT ${_SCHEMA_COMPONENT})

# version compatibility
add_subdirectory(versions)
