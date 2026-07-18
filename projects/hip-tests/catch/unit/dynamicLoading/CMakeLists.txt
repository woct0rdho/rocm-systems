# Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
#
# SPDX-License-Identifier: MIT

if(UNIX AND BUILD_SHARED_LIBS)
  set(TEST_SRC ${TEST_SRC}
      complex_loading_behavior.cc
      hipApiDynamicLoad.cc
      hipGetProcAddress.cc
      hipGetProcAddressCompilerOption.cc)

  if(HIP_PLATFORM MATCHES "amd")
    list(APPEND TEST_SRC hipLateDsoLifecycle.cc)
  endif()

  hip_add_exe_to_target(NAME dynamicLoading
                        TEST_SRC ${TEST_SRC}
                        TEST_TARGET_NAME build_tests)

  if(HIP_PLATFORM MATCHES "amd")
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/libLazyLoad.so
      COMMAND ${CMAKE_HIP_COMPILER} ${HIP_DEVICE_BUILD_FLAGS_NO_ARCH} ${OFFLOAD_ARCH_LIST} -fPIC -lpthread -shared
      -x hip ${CMAKE_CURRENT_SOURCE_DIR}/liblazyLoad.cc
      -I${CMAKE_CURRENT_SOURCE_DIR}/../../include ${HIP_PATH_OPT}
      -I${Catch2_SOURCE_DIR}/src -I${Catch2_BINARY_DIR}/generated-includes
      -o ${CMAKE_CURRENT_BINARY_DIR}/libLazyLoad.so
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/liblazyLoad.cc
      COMMENT "Compiling libLazyLoad.so")
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bit_extract_kernel.code
      COMMAND ${CMAKE_HIP_COMPILER} ${HIP_DEVICE_BUILD_FLAGS_NO_ARCH} ${OFFLOAD_ARCH_LIST} --cuda-device-only
      -x hip ${CMAKE_CURRENT_SOURCE_DIR}/bit_extract_kernel.cpp ${HIP_PATH_OPT}
      -o ${CMAKE_CURRENT_BINARY_DIR}/bit_extract_kernel.code
      -I${CMAKE_CURRENT_SOURCE_DIR}/../../include
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bit_extract_kernel.cpp
      COMMENT "Compiling bit_extract_kernel.code")

    set(HIP_LATE_DSO_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/hipLateDsoPlugin.cc)
    set(HIP_LATE_DSO_A ${CMAKE_CURRENT_BINARY_DIR}/libHipLateDsoA.so)
    set(HIP_LATE_DSO_B ${CMAKE_CURRENT_BINARY_DIR}/libHipLateDsoB.so)

    add_custom_command(
      OUTPUT ${HIP_LATE_DSO_A}
      COMMAND ${CMAKE_HIP_COMPILER} ${HIP_DEVICE_BUILD_FLAGS_NO_ARCH} ${OFFLOAD_ARCH_LIST}
              -fPIC -shared -x hip ${HIP_LATE_DSO_SOURCE} ${HIP_PATH_OPT}
              -I${HIP_PATH}/include -DHIP_LATE_DSO_ID=1 -o ${HIP_LATE_DSO_A}
      DEPENDS ${HIP_LATE_DSO_SOURCE}
      COMMENT "Building late-loaded HIP DSO A"
      VERBATIM)
    add_custom_command(
      OUTPUT ${HIP_LATE_DSO_B}
      COMMAND ${CMAKE_HIP_COMPILER} ${HIP_DEVICE_BUILD_FLAGS_NO_ARCH} ${OFFLOAD_ARCH_LIST}
              -fPIC -shared -x hip ${HIP_LATE_DSO_SOURCE} ${HIP_PATH_OPT}
              -I${HIP_PATH}/include -DHIP_LATE_DSO_ID=2 -o ${HIP_LATE_DSO_B}
      DEPENDS ${HIP_LATE_DSO_SOURCE}
      COMMENT "Building late-loaded HIP DSO B"
      VERBATIM)
    add_custom_target(HipLateDsoPlugins DEPENDS ${HIP_LATE_DSO_A} ${HIP_LATE_DSO_B})

    add_executable(hipLateDsoLifecycle_exe EXCLUDE_FROM_ALL hipLateDsoLifecycle_exe.cc)
    set_source_files_properties(hipLateDsoLifecycle_exe.cc PROPERTIES LANGUAGE ${GPGPU_LANGUAGE})
    target_link_libraries(hipLateDsoLifecycle_exe ${GPGPU_LINKER_LIBRARIES} ${CMAKE_DL_LIBS})
    add_dependencies(hipLateDsoLifecycle_exe HipLateDsoPlugins)
    set_property(GLOBAL APPEND PROPERTY G_INSTALL_EXE_TARGETS hipLateDsoLifecycle_exe)
    set_property(GLOBAL APPEND PROPERTY G_INSTALL_CUSTOM_TARGETS
      ${HIP_LATE_DSO_A}
      ${HIP_LATE_DSO_B})
  elseif(HIP_PLATFORM MATCHES "nvidia")
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/libLazyLoad.so
      COMMAND ${CMAKE_CUDA_COMPILER} -Xcompiler -fPIC -lpthread -shared
      -x cu ${CMAKE_CURRENT_SOURCE_DIR}/liblazyLoad.cc -I${CMAKE_CURRENT_SOURCE_DIR}/../../include
      -I${Catch2_SOURCE_DIR}/src -I${Catch2_BINARY_DIR}/generated-includes -I${HIP_PATH}/include/
      -o ${CMAKE_CURRENT_BINARY_DIR}/libLazyLoad.so
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/liblazyLoad.cc
      COMMENT "Compiling libLazyLoad.so")
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bit_extract_kernel.code
      COMMAND ${CMAKE_HIP_COMPILER} --fatbin
      -x cu ${CMAKE_CURRENT_SOURCE_DIR}/bit_extract_kernel.cpp -I${HIP_PATH}/include/
      ${OFFLOAD_ARCH_LIST} -o ${CMAKE_CURRENT_BINARY_DIR}/bit_extract_kernel.code
      -I${CMAKE_CURRENT_SOURCE_DIR}/../../include
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bit_extract_kernel.cpp
      COMMENT "Compiling bit_extract_kernel.code")
  endif()
  add_custom_target(libLazyLoad_so DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/libLazyLoad.so)
  add_custom_target(bit_extract_kernel_code DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bit_extract_kernel.code)

  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vecadd.cc
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_SOURCE_DIR}/vecadd.cc ${CMAKE_CURRENT_BINARY_DIR}/vecadd.cc
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vecadd.cc)
  add_custom_target(vecadd_cc DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/vecadd.cc)

  set_property(GLOBAL APPEND PROPERTY G_INSTALL_CUSTOM_TARGETS
    ${CMAKE_CURRENT_BINARY_DIR}/vecadd.cc
    ${CMAKE_CURRENT_BINARY_DIR}/libLazyLoad.so
    ${CMAKE_CURRENT_BINARY_DIR}/bit_extract_kernel.code)

  if(HIP_PLATFORM MATCHES "amd")
    set_source_files_properties(hipGetProcAddressCompilerOption.cc PROPERTIES COMPILE_OPTIONS "-fgpu-default-stream=per-thread")
  endif()

  # Wire artifacts to the consuming exe so `make dynamicLoading` builds them.
  add_dependencies(dynamicLoading bit_extract_kernel_code libLazyLoad_so vecadd_cc)
  if(HIP_PLATFORM MATCHES "amd")
    add_dependencies(dynamicLoading hipLateDsoLifecycle_exe)
    if(TARGET hipLateDsoLifecycle)
      add_dependencies(hipLateDsoLifecycle hipLateDsoLifecycle_exe)
    endif()
  endif()
endif()
