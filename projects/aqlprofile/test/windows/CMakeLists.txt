add_executable(aqlprofile-windows-loader-probe aqlprofile_loader_probe.cpp)
add_executable(aqlprofile-windows-pmc-packet-probe aqlprofile_pmc_packet_probe.cpp)

target_include_directories(
  aqlprofile-windows-loader-probe
  PRIVATE
    ${AQLPROFILE_HSA_INCLUDE_DIR}
)
set(_wddm_profiling_include "${PROJECT_SOURCE_DIR}/../rocr-runtime/libhsakmt/include")
if(NOT EXISTS "${_wddm_profiling_include}/impl/wddm/profiling.h")
  message(FATAL_ERROR "The Windows PMC packet probe requires the sibling ROCr WDDM adapter")
endif()
target_include_directories(
  aqlprofile-windows-pmc-packet-probe
  PRIVATE
    ${AQLPROFILE_HSA_INCLUDE_DIR}
    ${_wddm_profiling_include}
)

target_link_libraries(aqlprofile-windows-loader-probe PRIVATE aqlprofile::headers)
target_link_libraries(aqlprofile-windows-pmc-packet-probe PRIVATE aqlprofile::headers)
add_dependencies(aqlprofile-windows-pmc-packet-probe ${AQLPROFILE_TARGET})

set_target_properties(
  aqlprofile-windows-loader-probe
  aqlprofile-windows-pmc-packet-probe
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
)

add_test(
  NAME aqlprofile.windows.loader
  COMMAND aqlprofile-windows-loader-probe
          $<TARGET_FILE:${AQLPROFILE_TARGET}>
          ${AQLPROFILE_WINDOWS_HSA_RUNTIME}
)
set_tests_properties(
  aqlprofile.windows.loader
  PROPERTIES
    LABELS "windows;integration;gpu"
    TIMEOUT 30
    PASS_REGULAR_EXPRESSION
      "PASS: direct DLL loading and HSA extension loading succeeded"
)

add_test(
  NAME aqlprofile.windows.pmc-packet.gfx1151
  COMMAND aqlprofile-windows-pmc-packet-probe
          $<TARGET_FILE:${AQLPROFILE_TARGET}>
          ${AQLPROFILE_WINDOWS_HSA_RUNTIME}
)
set_tests_properties(
  aqlprofile.windows.pmc-packet.gfx1151
  PROPERTIES
    LABELS "windows;unit;cpu-only;pmc"
    TIMEOUT 30
    PASS_REGULAR_EXPRESSION
      "PASS: gfx1151 PMC packets constructed without HSA initialization"
)
