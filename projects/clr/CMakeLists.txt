# Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16.8)
project(clr)

##########
# Defaults
##########
if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"
    OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_compile_options("/wd4267" "/wd4244" "/wd4996")
    # Enable parallel compilation on MSVC.
    if(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL})
        add_compile_options("/MP$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
    endif()
    string(REPLACE "/GR" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    string(REPLACE "/W3" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(/MTd)
        add_compile_options(-D_ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH)
    else()
        if (AMD_COMPUTE_WIN)
            add_compile_options(/MT)
        endif()
    endif()
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # Unlike when targeting MSFT's MSVC, when targeting clang-cl,
        # CMake does not add these automatically.  Add them ourselves.
        add_compile_options("/DWIN32" "/D_WINDOWS" "/EHsc")
    endif()
endif()

if (AMD_COMPUTE_WIN)
    set(AMD_LIBELF_PATH "${AMD_COMPUTE_WIN}/hsail-compiler/lib/loaders/elf/utils/libelf")
    set(AMD_SC_PATH "${AMD_COMPUTE_WIN}/sc")
    message("Public Compute Windows build path: ${AMD_COMPUTE_WIN}, SC: ${AMD_SC_PATH}, LibElf: ${AMD_LIBELF_PATH}")
endif()

option(CLR_BUILD_HIP "Build HIP" OFF)
option(CLR_BUILD_OCL "Build OCL" OFF)
option(USE_NEW_HOSTCALL_IMPL
  "Use the new phase/occupancy-based hostcall implementation" OFF)
option(ROCCLR_BUILD_WINDOWS_TESTS "Build CPU-only Windows CLR/ROCr tests" OFF)

if(WIN32 AND ROCCLR_BUILD_WINDOWS_TESTS)
    enable_testing()
    include(CTest)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

# Set default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

#############
# Build steps
#############

if(NOT DEFINED ROCM_PATH)
  if(DEFINED ENV{ROCM_PATH})
    set(ROCM_PATH "$ENV{ROCM_PATH}" CACHE STRING "ROCm installation directory.")
  elseif(CMAKE_PREFIX_PATH)
    list(GET CMAKE_PREFIX_PATH 0 _first_prefix_path)
    set(ROCM_PATH "${_first_prefix_path}" CACHE STRING "ROCm installation directory.")
    unset(_first_prefix_path)
  else()
    set(ROCM_PATH "${CMAKE_INSTALL_PREFIX}" CACHE STRING "ROCm installation directory.")
  endif()
endif()
message(STATUS "ROCM_PATH: ${ROCM_PATH}")

# Attempt to auto-detect HIP_PLATFORM by interrogating hipconfig. This is kept
# for compatibility: users are advised to pass HIP_PLATFORM explicitly.
# Sets the HIP_PLATFORM variable in the parent scope.
function(_hip_clr_auto_detect_hip_platform)
    if (UNIX)
      set(HIPCC_EXECUTABLE "hipcc")
      set(HIPCONFIG_EXECUTABLE "hipconfig")
    else()
      set(HIPCC_EXECUTABLE "hipcc.exe")
      set(HIPCONFIG_EXECUTABLE "hipconfig.exe")
    endif()

    # Set default HIPCC_BIN_DIR to ${ROCM_PATH}/bin
    if(NOT DEFINED HIPCC_BIN_DIR AND UNIX)
        set(HIPCC_BIN_DIR "${ROCM_PATH}/bin")
    endif()
    message(STATUS "Auto-detect HIP_PLATFORM from HIPCC Binary Directory: ${HIPCC_BIN_DIR}")

    if(NOT EXISTS ${HIPCC_BIN_DIR}/${HIPCONFIG_EXECUTABLE})
        message(FATAL_ERROR "Please pass hipcc/build or hipcc/bin using -DHIPCC_BIN_DIR.")
    endif()

    # Determine HIP_PLATFORM
    if(NOT DEFINED HIP_PLATFORM)
        if(NOT DEFINED ENV{HIP_PLATFORM})
            execute_process(COMMAND ${HIPCC_BIN_DIR}/${HIPCONFIG_EXECUTABLE} --platform
                OUTPUT_VARIABLE _detected_hip_platform
                RESULT_VARIABLE result
                OUTPUT_STRIP_TRAILING_WHITESPACE)

	    if(result AND NOT result EQUAL 0)
                message(FATAL_ERROR "Failure trying to auto-detect HIP_PLATFORM via command: ${HIPCC_BIN_DIR}/${HIPCONFIG_EXECUTABLE} --platform: ${result}")
	    endif()

            set(HIP_PLATFORM "${_detected_hip_platform}" PARENT_SCOPE)
        else()
            set(HIP_PLATFORM "$ENV{HIP_PLATFORM}" PARENT_SCOPE)
        endif()
    endif()
endfunction()
if(CLR_BUILD_HIP)
    message(STATUS "HIP Common Directory: ${HIP_COMMON_DIR}")
    if(NOT DEFINED HIP_COMMON_DIR)
        message(FATAL_ERROR "Please pass HIP using -DHIP_COMMON_DIR. HIP_COMMON_DIR is incorrect")
    endif()
    if(NOT DEFINED HIP_PLATFORM)
        _hip_clr_auto_detect_hip_platform()
    endif()
endif()

if(CLR_BUILD_HIP)
    option(BUILD_SHARED_LIBS "Build the shared library" ON)
    if (NOT BUILD_SHARED_LIBS)
        add_compile_definitions(BUILD_STATIC_LIBS)
    endif()
endif()
if((CLR_BUILD_HIP AND HIP_PLATFORM STREQUAL "amd") OR CLR_BUILD_OCL)
    add_subdirectory(rocclr)
elseif(HIP_PLATFORM STREQUAL "amd")
    message(FATAL_ERROR "Please enable building of one or more of the below runtimes:\n- HIP (-DCLR_BUILD_HIP=ON)\n- OpenCL (-DCLR_BUILD_OCL-ON)")
endif()
if(CLR_BUILD_HIP)
    add_subdirectory(hipamd)
endif()

if(WIN32 AND CLR_BUILD_HIP AND ROCCLR_BUILD_WINDOWS_TESTS)
    find_package(rocprofiler-register CONFIG REQUIRED)

    add_test(NAME clr.windows.runtime-binary
        COMMAND Python3::Interpreter
                ${CMAKE_CURRENT_SOURCE_DIR}/rocclr/device/rocm/tests/runtime_validation.py
                --runtime $<TARGET_FILE:amdhip64>
                --build-directory ${CMAKE_BINARY_DIR}
                --cache ${CMAKE_BINARY_DIR}/CMakeCache.txt
                --output ${CMAKE_BINARY_DIR}/validation/hip-runtime-build.txt)
    set_tests_properties(clr.windows.runtime-binary PROPERTIES
        LABELS "windows;unit;cpu-only;pe"
        TIMEOUT 30
        PASS_REGULAR_EXPRESSION "hip_runtime_validation=passed")

    add_library(clr-windows-registration-profiler SHARED
        rocclr/device/rocm/tests/windows_registration_profiler.cpp)
    set_target_properties(clr-windows-registration-profiler PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        OUTPUT_NAME rocprofiler-sdk)

    add_executable(clr-windows-registration-test
        rocclr/device/rocm/tests/windows_registration_test.cpp)
    target_link_libraries(clr-windows-registration-test PRIVATE
        rocprofiler-register::rocprofiler-register)
    set_target_properties(clr-windows-registration-test PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_compile_options(clr-windows-registration-test PRIVATE /EHsc)

    foreach(_registration_mode normal late)
        add_test(NAME clr.windows.registration.${_registration_mode}
            COMMAND $<TARGET_FILE:clr-windows-registration-test>
                    $<TARGET_FILE:clr-windows-registration-profiler>
                    $<TARGET_FILE:amdhip64>
                    $<TARGET_FILE:rocprofiler-register::rocprofiler-register>
                    ${_registration_mode})
        set_tests_properties(clr.windows.registration.${_registration_mode} PROPERTIES
            LABELS "windows;unit;cpu-only;registration"
            TIMEOUT 30
            PASS_REGULAR_EXPRESSION
                "windows_hip_registration=${_registration_mode} passed hsa_loaded=no gpu_work_executed=no")
        if(ROCM_KPACK_ENABLED)
            set_property(TEST clr.windows.registration.${_registration_mode} APPEND PROPERTY
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${ROCM_PATH}/bin")
        endif()
    endforeach()
endif()
if(CLR_BUILD_OCL)
    add_subdirectory(opencl)
endif()

#############################
# Code formatting
#############################
# Target: clangformat
find_program(CLANGFORMAT_EXE clang-format PATHS "${ROCM_PATH}/lib/llvm/bin")
if(CLANGFORMAT_EXE)
    file(GLOB_RECURSE FORMAT_SOURCE_FILE_LIST *.cpp *.hpp *.hh *.h *.cc *.c)
    add_custom_target(clangformat COMMAND ${CLANGFORMAT_EXE} -style=file -i ${FORMAT_SOURCE_FILE_LIST}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
