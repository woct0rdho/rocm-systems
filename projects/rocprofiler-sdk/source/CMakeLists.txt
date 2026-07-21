#
#
#
if(ROCPROFILER_BUILD_CODECOV)
    set(CMAKE_BUILD_TYPE "Coverage")
endif()

#
# by default, activate clang-tidy on all code in the source folder. unittest subfolders
# can add `rocprofiler_deactivate_clang_tidy()` to their CMakeLists.txt to disable
# clang-tidy linting
#
rocprofiler_activate_clang_tidy()

add_subdirectory(include)
add_subdirectory(lib)
if(NOT ROCPROFILER_BUILD_WINDOWS_MINIMAL)
    add_subdirectory(libexec)
endif()
add_subdirectory(bin)
if(NOT ROCPROFILER_BUILD_WINDOWS_MINIMAL)
    add_subdirectory(docs)
endif()
add_subdirectory(share)
