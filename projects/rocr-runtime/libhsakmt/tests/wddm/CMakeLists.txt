# ##############################################################################
#
# Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
#
# SPDX-License-Identifier: MIT
#
# ##############################################################################

# CmdbufFrameRing is pure arithmetic over the submission ordinal and includes
# nothing but <cstdint>, so this test needs neither the Windows SDK nor the DXG
# interop headers and runs on the build host.
add_executable(cmdbuf_frame_ring_test cmdbuf_frame_ring_test.cpp)

target_include_directories(cmdbuf_frame_ring_test
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

target_compile_features(cmdbuf_frame_ring_test PRIVATE cxx_std_17)

add_test(NAME cmdbuf_frame_ring COMMAND cmdbuf_frame_ring_test)

# Validates the built runtime without loading HSA: PE machine type, static MSVC
# runtime, ROCr's required and PC-sampling-disabled exports, the profiler-register
# dependency, and the cache values that define the Windows service boundary.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(rocr-windows-queue-profiling-test queue_profiling.cpp)
target_compile_features(rocr-windows-queue-profiling-test PRIVATE cxx_std_20)
target_include_directories(rocr-windows-queue-profiling-test PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../../runtime)
add_test(NAME rocr.windows.queue-profiling
  COMMAND rocr-windows-queue-profiling-test)
set_tests_properties(rocr.windows.queue-profiling PROPERTIES
  LABELS "windows;unit;cpu-only;profiling;wddm"
  TIMEOUT 30
  PASS_REGULAR_EXPRESSION
    "queue_profiling=passed initially_disabled=1 enabled=1 disabled=1")

if(TARGET hsa-runtime64)
  set(_rocr_windows_runtime_validation
      ${CMAKE_BINARY_DIR}/validation/runtime-build.txt)
  add_test(NAME rocr.windows.runtime-binary
    COMMAND Python3::Interpreter
            ${CMAKE_CURRENT_LIST_DIR}/runtime_validation.py
            --runtime $<TARGET_FILE:hsa-runtime64>
            --cache ${CMAKE_BINARY_DIR}/CMakeCache.txt
            --output ${_rocr_windows_runtime_validation})
  set_tests_properties(rocr.windows.runtime-binary PROPERTIES
    LABELS "windows;unit;cpu-only;pe"
    TIMEOUT 30
    PASS_REGULAR_EXPRESSION "runtime_validation=passed")
endif()
