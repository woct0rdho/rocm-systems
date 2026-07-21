#
#
#
rocprofiler_activate_clang_tidy()

set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "core")
add_subdirectory(common)
if(NOT ROCPROFILER_BUILD_WINDOWS_MINIMAL)
    add_subdirectory(output)
endif()
add_subdirectory(aqlprofile)
add_subdirectory(rocprofiler-sdk)
if(NOT ROCPROFILER_BUILD_WINDOWS_MINIMAL)
    add_subdirectory(rocprofiler-sdk-attach)
endif()

set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "roctx")
add_subdirectory(rocprofiler-sdk-roctx)

if(NOT ROCPROFILER_BUILD_WINDOWS_MINIMAL)
    set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "rocpd")
    add_subdirectory(rocprofiler-sdk-rocpd)

    # rocattach is only supported on x86_64 for now
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
        set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "rocattach")
        add_subdirectory(rocprofiler-sdk-rocattach)
    endif()

    set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "tools")
    add_subdirectory(att-tool)
    add_subdirectory(rocprofiler-sdk-tool)
else()
    set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "tools")
    add_subdirectory(rocprofiler-sdk-tool)
endif()

add_subdirectory(python)

if(ROCPROFILER_BUILD_TESTS)
    add_subdirectory(tests)
endif()
