cmake_minimum_required(VERSION 3.21)
project(hsakmt_windows_tests LANGUAGES CXX)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_hsakmt_wddm_device_source
    ${CMAKE_CURRENT_LIST_DIR}/../../src/dxg/wddm/device.cpp)
set(_hsakmt_wddm_profiling_header
    ${CMAKE_CURRENT_LIST_DIR}/../../include/impl/wddm/profiling.h)
file(READ ${_hsakmt_wddm_device_source} _hsakmt_wddm_device_text)
file(READ ${_hsakmt_wddm_profiling_header} _hsakmt_wddm_profiling_text)
if(NOT _hsakmt_wddm_device_text MATCHES
       "gfx11_aql_profile_frame_size[ \t\r\n]*=[ \t\r\n]*profiling::kQualifiedFrameBytes" OR
   NOT _hsakmt_wddm_profiling_text MATCHES
       "kQualifiedFrameBytes[ \t\r\n]*=[ \t\r\n]*0x2000")
  message(FATAL_ERROR
    "The qualified Windows gfx11 AQL Profile WDDM frame must remain 0x2000 bytes")
endif()
unset(_hsakmt_wddm_profiling_text)
unset(_hsakmt_wddm_device_text)
unset(_hsakmt_wddm_profiling_header)
unset(_hsakmt_wddm_device_source)

set(_hsakmt_wddm_openclose_source
    ${CMAKE_CURRENT_LIST_DIR}/../../src/dxg/openclose.cpp)
file(READ ${_hsakmt_wddm_openclose_source} _hsakmt_wddm_openclose_text)
if(_hsakmt_wddm_openclose_text MATCHES
   "getenv\\(\"(ROCR_USE_PM4|WSLKMT_VENDOR_PACKET)\"")
  message(FATAL_ERROR
    "WDDM profiling activation must not be controlled by PM4 environment variables")
endif()
unset(_hsakmt_wddm_openclose_text)
unset(_hsakmt_wddm_openclose_source)

add_executable(hsakmt-windows-packet-publication-test packet_publication.cpp)
target_compile_features(hsakmt-windows-packet-publication-test PRIVATE cxx_std_20)
target_include_directories(hsakmt-windows-packet-publication-test PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../../runtime/hsa-runtime)

add_test(NAME hsakmt.windows.packet-publication
  COMMAND hsakmt-windows-packet-publication-test)
set_tests_properties(hsakmt.windows.packet-publication PROPERTIES
  LABELS "windows;unit;cpu-only;wddm"
  TIMEOUT 30
  PASS_REGULAR_EXPRESSION "packet publication iterations=200000 failures=0")

add_executable(hsakmt-windows-profiling-adapter-test profiling_adapter.cpp)
target_compile_features(hsakmt-windows-profiling-adapter-test PRIVATE cxx_std_20)
target_include_directories(hsakmt-windows-profiling-adapter-test PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../include
  ${CMAKE_CURRENT_LIST_DIR}/../../../runtime
  ${CMAKE_CURRENT_LIST_DIR}/../../../runtime/hsa-runtime)

add_test(NAME hsakmt.windows.profiling-adapter
  COMMAND hsakmt-windows-profiling-adapter-test)
set_tests_properties(hsakmt.windows.profiling-adapter PROPERTIES
  LABELS "windows;unit;cpu-only;wddm;pmc"
  TIMEOUT 30
  PASS_REGULAR_EXPRESSION
    "profiling_adapter checks=46 failures=0 capability_version=1 max_dwords=256")

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
