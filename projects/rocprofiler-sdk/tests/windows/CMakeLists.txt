find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_windows_pytest_runner ${CMAKE_CURRENT_LIST_DIR}/common/run_pytest.py)

add_test(
    NAME rocprofiler-sdk.windows.rocprofv3.unit
    COMMAND
        Python3::Interpreter ${_windows_pytest_runner}
        --owner-root ${PROJECT_BINARY_DIR}
        --basetemp ${PROJECT_BINARY_DIR}/pytest-rocprofv3-unit
        -- ${CMAKE_CURRENT_LIST_DIR}/rocprofv3/test_windows_cli.py)
set_tests_properties(
    rocprofiler-sdk.windows.rocprofv3.unit
    PROPERTIES
        LABELS "windows;unit;cpu-only;rocprofv3"
        TIMEOUT 60
        ENVIRONMENT
            "ROCPROFV3_TEST_SCRIPT=${PROJECT_SOURCE_DIR}/source/bin/rocprofv3.py;PYTHONDONTWRITEBYTECODE=1")

set(_pmc_parity_test_directory ${CMAKE_CURRENT_LIST_DIR}/../pmc-parity)
add_test(
    NAME rocprofiler-sdk.windows.pmc-contract
    COMMAND
        Python3::Interpreter ${_windows_pytest_runner}
        --owner-root ${PROJECT_BINARY_DIR}
        --basetemp ${PROJECT_BINARY_DIR}/pytest-pmc-contract
        -- ${_pmc_parity_test_directory}/test_pmc_contract.py)
set_tests_properties(
    rocprofiler-sdk.windows.pmc-contract
    PROPERTIES
        LABELS "windows;unit;cpu-only;pmc"
        TIMEOUT 60
        ENVIRONMENT
            "ROCPROFILER_PMC_GFX1151_CONTRACT=${_pmc_parity_test_directory}/gfx1151_linux_contract.json;PYTHONDONTWRITEBYTECODE=1")

add_test(
    NAME rocprofiler-sdk.windows.aqlprofile-mirror
    COMMAND
        Python3::Interpreter ${_windows_pytest_runner}
        --owner-root ${PROJECT_BINARY_DIR}
        --basetemp ${PROJECT_BINARY_DIR}/pytest-aqlprofile-mirror
        -- ${CMAKE_CURRENT_LIST_DIR}/common/test_aqlprofile_mirror.py)
set_tests_properties(
    rocprofiler-sdk.windows.aqlprofile-mirror
    PROPERTIES
        LABELS "windows;unit;cpu-only;aqlprofile"
        TIMEOUT 30
        ENVIRONMENT
            "ROCPROFILER_TEST_REPOSITORY_ROOT=${PROJECT_SOURCE_DIR}/../..;PYTHONDONTWRITEBYTECODE=1")

add_test(
    NAME rocprofiler-sdk.windows.build-prefix
    COMMAND
        Python3::Interpreter ${_windows_pytest_runner}
        --owner-root ${PROJECT_BINARY_DIR}
        --basetemp ${PROJECT_BINARY_DIR}/pytest-build-prefix
        -- ${CMAKE_CURRENT_LIST_DIR}/common/test_windows_build_prefix.py)
set_tests_properties(
    rocprofiler-sdk.windows.build-prefix
    PROPERTIES
        LABELS "windows;unit;cpu-only;build"
        TIMEOUT 30
        ENVIRONMENT
            "ROCPROFILER_TEST_REPOSITORY_ROOT=${PROJECT_SOURCE_DIR}/../..;PYTHONDONTWRITEBYTECODE=1")

add_test(
    NAME rocprofiler-sdk.windows.rocprofv3.process
    COMMAND ${Python3_EXECUTABLE} ${_windows_pytest_runner}
            --owner-root ${PROJECT_BINARY_DIR}
            --basetemp ${PROJECT_BINARY_DIR}/pytest-rocprofv3-process
            -- ${CMAKE_CURRENT_LIST_DIR}/rocprofv3/test_windows_process_cli.py)
set_tests_properties(
    rocprofiler-sdk.windows.rocprofv3.process
    PROPERTIES
        LABELS "windows;integration;cpu-only;rocprofv3"
        TIMEOUT 60
        ENVIRONMENT
            "ROCPROFV3_TEST_PYTHON=${Python3_EXECUTABLE};ROCPROFV3_TEST_BUILT_SCRIPT=${PROJECT_BINARY_DIR}/bin/rocprofv3;PYTHONDONTWRITEBYTECODE=1")

add_test(
    NAME rocprofiler-sdk.windows.job-object
    COMMAND ${Python3_EXECUTABLE} ${_windows_pytest_runner}
            --owner-root ${PROJECT_BINARY_DIR}
            --basetemp ${PROJECT_BINARY_DIR}/pytest-windows-job
            -- ${CMAKE_CURRENT_LIST_DIR}/common/test_windows_job.py)
set_tests_properties(
    rocprofiler-sdk.windows.job-object
    PROPERTIES
        LABELS "windows;unit;cpu-only;process"
        TIMEOUT 30
        ENVIRONMENT
            "PYTHONPATH=${CMAKE_CURRENT_LIST_DIR}/common;PYTHONDONTWRITEBYTECODE=1")

if(ROCPROFILER_BUILD_WINDOWS_MINIMAL)
    if(NOT IS_ABSOLUTE "${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}" OR
       NOT EXISTS "${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/amdhip64_7.dll")
        message(FATAL_ERROR
                "ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT must contain bin/amdhip64_7.dll")
    endif()

    find_package(rocprofiler-register CONFIG REQUIRED)
    add_executable(rocprofiler-sdk-windows-registration registration_test.cpp)
    add_executable(rocprofiler-sdk-windows-registration-reentrancy
                   registration_reentrancy_test.cpp)
    add_executable(rocprofiler-sdk-windows-registration-concurrent
                   registration_concurrent_test.cpp)
    add_executable(rocprofiler-sdk-windows-registration-multi-client
                   registration_multi_client_test.cpp)
    foreach(_client_tag first second)
        add_library(rocprofiler-sdk-windows-registration-${_client_tag}-tool SHARED
                    registration_multi_client_tool.cpp)
        set_target_properties(
            rocprofiler-sdk-windows-registration-${_client_tag}-tool
            PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
        target_compile_features(
            rocprofiler-sdk-windows-registration-${_client_tag}-tool PRIVATE cxx_std_17)
        target_include_directories(
            rocprofiler-sdk-windows-registration-${_client_tag}-tool
            PRIVATE ${PROJECT_SOURCE_DIR}/source)
        target_compile_definitions(
            rocprofiler-sdk-windows-registration-${_client_tag}-tool
            PRIVATE ROCPROFILER_TEST_TOOL_TAG="${_client_tag}")
        target_link_libraries(
            rocprofiler-sdk-windows-registration-${_client_tag}-tool
            PRIVATE rocprofiler-sdk::rocprofiler-sdk-headers
                    rocprofiler-sdk::rocprofiler-sdk-build-flags)
    endforeach()
    add_dependencies(
        rocprofiler-sdk-windows-registration-multi-client
        rocprofiler-sdk-windows-registration-first-tool
        rocprofiler-sdk-windows-registration-second-tool)
    add_executable(rocprofiler-sdk-windows-roctx-workload roctx_workload.cpp)
    target_compile_features(rocprofiler-sdk-windows-registration PRIVATE cxx_std_17)
    target_link_libraries(
        rocprofiler-sdk-windows-registration
        PRIVATE rocprofiler-sdk::rocprofiler-sdk-build-flags)
    foreach(_registration_target
            rocprofiler-sdk-windows-registration-reentrancy
            rocprofiler-sdk-windows-registration-concurrent
            rocprofiler-sdk-windows-registration-multi-client)
        target_compile_features(${_registration_target} PRIVATE cxx_std_17)
        target_link_libraries(
            ${_registration_target}
            PRIVATE rocprofiler-sdk::rocprofiler-sdk-shared-library
                    rocprofiler-sdk::rocprofiler-sdk-headers
                    rocprofiler-sdk::rocprofiler-sdk-build-flags)
        add_custom_command(
            TARGET ${_registration_target}
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rocprofiler-register::rocprofiler-register>
                $<TARGET_FILE_DIR:${_registration_target}>)
    endforeach()
    target_link_libraries(
        rocprofiler-sdk-windows-roctx-workload
        PRIVATE rocprofiler-sdk::rocprofiler-sdk-build-flags
                rocprofiler-sdk::rocprofiler-sdk-roctx-shared-library)

    set(_windows_install_validation_root
        ${ROCPROFILER_WINDOWS_TEST_OUTPUT_ROOT})
    if(NOT _windows_install_validation_root)
        set(_windows_install_validation_root
            ${PROJECT_BINARY_DIR}/validation/installed-prefix)
    endif()
    add_test(
        NAME rocprofiler-sdk.windows.installed-prefix
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/validate_windows_install.py
            --prefix ${ROCPROFILER_WINDOWS_TEST_INSTALL_PREFIX}
            --output ${_windows_install_validation_root}
            --fresh
            --roctx-workload $<TARGET_FILE:rocprofiler-sdk-windows-roctx-workload>)
    set_tests_properties(
        rocprofiler-sdk.windows.installed-prefix
        PROPERTIES
            LABELS "windows;integration;cpu-only;install;rocprofv3;roctx"
            TIMEOUT 60
            ENVIRONMENT
                "PYTHONPATH=${CMAKE_CURRENT_LIST_DIR}/common;PYTHONDONTWRITEBYTECODE=1")

    add_test(
        NAME rocprofiler-sdk.windows.registration.reentrancy
        COMMAND rocprofiler-sdk-windows-registration-reentrancy)
    set_tests_properties(
        rocprofiler-sdk.windows.registration.reentrancy
        PROPERTIES
            LABELS "windows;unit;cpu-only;registration"
            TIMEOUT 30)
    add_test(
        NAME rocprofiler-sdk.windows.registration.concurrent
        COMMAND rocprofiler-sdk-windows-registration-concurrent)
    set_tests_properties(
        rocprofiler-sdk.windows.registration.concurrent
        PROPERTIES
            LABELS "windows;unit;cpu-only;registration;concurrent"
            TIMEOUT 30)
    add_test(
        NAME rocprofiler-sdk.windows.registration.multi-client
        COMMAND
            rocprofiler-sdk-windows-registration-multi-client
            $<TARGET_FILE:rocprofiler-sdk-windows-registration-first-tool>
            $<TARGET_FILE:rocprofiler-sdk-windows-registration-second-tool>
            ${PROJECT_BINARY_DIR}/validation/windows-sdk-registration-multi-client.txt)
    set_tests_properties(
        rocprofiler-sdk.windows.registration.multi-client
        PROPERTIES
            LABELS "windows;unit;cpu-only;registration;multi-client"
            TIMEOUT 30)
    add_test(
        NAME rocprofiler-sdk.windows.registration.finalization-failure
        COMMAND
            rocprofiler-sdk-windows-registration-multi-client
            $<TARGET_FILE:rocprofiler-sdk-windows-registration-first-tool>
            $<TARGET_FILE:rocprofiler-sdk-windows-registration-second-tool>
            ${PROJECT_BINARY_DIR}/validation/windows-sdk-registration-finalization-failure.txt
            throw-first)
    set_tests_properties(
        rocprofiler-sdk.windows.registration.finalization-failure
        PROPERTIES
            LABELS "windows;unit;cpu-only;registration;multi-client;failure"
            TIMEOUT 30)

    foreach(_mode normal late)
        set(_trace_file
            ${PROJECT_BINARY_DIR}/validation/windows-sdk-registration-${_mode}.txt)
        add_test(
            NAME rocprofiler-sdk.windows.registration.${_mode}
            COMMAND
                rocprofiler-sdk-windows-registration
                $<TARGET_FILE:rocprofiler-sdk-shared-library>
                ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/amdhip64_7.dll
                $<TARGET_FILE:rocprofiler-sdk-roctx-shared-library>
                $<TARGET_FILE:rocprofiler-register::rocprofiler-register>
                ${_trace_file}
                ${_mode})
        set_tests_properties(
            rocprofiler-sdk.windows.registration.${_mode}
            PROPERTIES
                LABELS "windows;integration;cpu-only;registration;hip;roctx"
                TIMEOUT 30
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin")
    endforeach()
endif()

if(ROCPROFILER_BUILD_WINDOWS_INTEGRATION_TESTS)
    foreach(_required_path
            ROCPROFILER_WINDOWS_TEST_VENV_ROOT
            ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT)
        if(NOT IS_ABSOLUTE "${${_required_path}}" OR NOT EXISTS "${${_required_path}}")
            message(FATAL_ERROR "${_required_path} must name an existing absolute path")
        endif()
    endforeach()

    find_program(
        ROCPROFILER_WINDOWS_TEST_HIPCC
        NAMES hipcc.exe
        HINTS
            ${ROCPROFILER_WINDOWS_TEST_VENV_ROOT}/Lib/site-packages/_rocm_sdk_devel/bin
        NO_DEFAULT_PATH
        REQUIRED)

    set(_windows_workload_source
        ${PROJECT_SOURCE_DIR}/tests/bin/windows-hip-workload/hip_workload.cpp)
    set(_windows_workload_compat
        ${PROJECT_SOURCE_DIR}/tests/bin/windows-hip-workload/compat)
    set(ROCPROFILER_WINDOWS_HIP_CMATH_COMPAT AUTO CACHE STRING
        "Use the scoped Clang HIP/MSVC <cmath> compatibility header: AUTO, ON, or OFF")
    set_property(CACHE ROCPROFILER_WINDOWS_HIP_CMATH_COMPAT PROPERTY STRINGS AUTO ON OFF)
    if(NOT ROCPROFILER_WINDOWS_HIP_CMATH_COMPAT MATCHES "^(AUTO|ON|OFF)$")
        message(FATAL_ERROR
                "ROCPROFILER_WINDOWS_HIP_CMATH_COMPAT must be AUTO, ON, or OFF")
    endif()
    set(_windows_workload_compat_enabled FALSE)
    if(ROCPROFILER_WINDOWS_HIP_CMATH_COMPAT STREQUAL "AUTO")
        set(_windows_cmath_probe_source
            ${PROJECT_BINARY_DIR}/tests/windows/hip-cmath-probe.cpp)
        set(_windows_cmath_probe_object
            ${PROJECT_BINARY_DIR}/tests/windows/hip-cmath-probe.obj)
        file(WRITE ${_windows_cmath_probe_source}
             "#include <hip/hip_runtime.h>\nint main() { return 0; }\n")
        execute_process(
            COMMAND ${ROCPROFILER_WINDOWS_TEST_HIPCC}
                    ${_windows_cmath_probe_source}
                    --offload-arch=gfx1151
                    -c
                    -o ${_windows_cmath_probe_object}
            RESULT_VARIABLE _windows_cmath_probe_result
            OUTPUT_VARIABLE _windows_cmath_probe_stdout
            ERROR_VARIABLE _windows_cmath_probe_stderr)
        file(WRITE ${PROJECT_BINARY_DIR}/tests/windows/hip-cmath-probe.txt
             "result=${_windows_cmath_probe_result}\n"
             "stdout:\n${_windows_cmath_probe_stdout}\n"
             "stderr:\n${_windows_cmath_probe_stderr}\n")
        if(NOT _windows_cmath_probe_result EQUAL 0)
            set(_windows_workload_compat_enabled TRUE)
            message(STATUS
                    "Clang HIP/MSVC <cmath> probe failed; enabling the scoped test-only compatibility header")
        endif()
    elseif(ROCPROFILER_WINDOWS_HIP_CMATH_COMPAT STREQUAL "ON")
        set(_windows_workload_compat_enabled TRUE)
    endif()
    set(_windows_workload_compat_args)
    set(_windows_workload_compat_dependencies)
    if(_windows_workload_compat_enabled)
        list(APPEND _windows_workload_compat_args -I ${_windows_workload_compat})
        list(APPEND _windows_workload_compat_dependencies
             ${_windows_workload_compat}/cmath)
    endif()
    set(_windows_workload_resource
        ${PROJECT_SOURCE_DIR}/tests/bin/windows-hip-workload/target-resource.txt)
    set(_windows_workload
        ${PROJECT_BINARY_DIR}/tests/windows/bin/rocprofiler-sdk-windows-hip-workload.exe)
    add_custom_command(
        OUTPUT ${_windows_workload}
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${PROJECT_BINARY_DIR}/tests/windows/bin
        COMMAND ${ROCPROFILER_WINDOWS_TEST_HIPCC}
                ${_windows_workload_source}
                -O2
                --offload-arch=gfx1151
                --rocm-device-lib-path=${ROCM_PATH}/lib/llvm/amdgcn/bitcode
                -DROCTX_IMPORT_DECORATOR=
                ${_windows_workload_compat_args}
                -I ${PROJECT_SOURCE_DIR}/source/include
                -I ${PROJECT_BINARY_DIR}/source/include
                -L ${PROJECT_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}
                -lrocprofiler-sdk-roctx
                -o ${_windows_workload}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/amdhip64_7.dll
                ${PROJECT_BINARY_DIR}/tests/windows/bin
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/amd_comgr.dll
                ${PROJECT_BINARY_DIR}/tests/windows/bin
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/hsa-amd-aqlprofile64.dll
                ${PROJECT_BINARY_DIR}/tests/windows/bin
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/rocprofiler-register.dll
                ${PROJECT_BINARY_DIR}/tests/windows/bin
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rocprofiler-sdk-roctx-shared-library>
                ${PROJECT_BINARY_DIR}/tests/windows/bin
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_windows_workload_resource}
                ${PROJECT_BINARY_DIR}/tests/windows/bin
        DEPENDS
            ${_windows_workload_source}
            ${_windows_workload_compat_dependencies}
            ${_windows_workload_resource}
            ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/amdhip64_7.dll
            ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/amd_comgr.dll
            ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/hsa-amd-aqlprofile64.dll
            ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/rocprofiler-register.dll
            rocprofiler-sdk-roctx-shared-library
        VERBATIM)
    add_custom_target(
        rocprofiler-sdk-windows-hip-workload ALL
        DEPENDS ${_windows_workload})

    add_test(
        NAME rocprofiler-sdk.windows.rocprofv3.pmc-process
        COMMAND ${Python3_EXECUTABLE} ${_windows_pytest_runner}
                --owner-root ${PROJECT_BINARY_DIR}
                --basetemp ${PROJECT_BINARY_DIR}/pytest-rocprofv3-pmc-process
                -- ${CMAKE_CURRENT_LIST_DIR}/rocprofv3/test_windows_pmc_process_cli.py)
    set_tests_properties(
        rocprofiler-sdk.windows.rocprofv3.pmc-process
        PROPERTIES
            LABELS "windows;integration;gpu;rocprofv3;counters"
            RESOURCE_LOCK windows_gpu
            TIMEOUT 240
            ENVIRONMENT
                "ROCPROFV3_TEST_PYTHON=${Python3_EXECUTABLE};ROCPROFV3_TEST_BUILT_SCRIPT=${PROJECT_BINARY_DIR}/bin/rocprofv3;ROCPROFV3_TEST_WORKLOAD=${_windows_workload};ROCPROFILER_WINDOWS_SDK_DLL=$<TARGET_FILE:rocprofiler-sdk-shared-library>;ROCPROFILER_WINDOWS_TOOL_DLL=$<TARGET_FILE:rocprofiler-sdk-tool>;ROCPROFILER_WINDOWS_CORE_BIN=${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin;PYTHONDONTWRITEBYTECODE=1")

    set(_windows_dispatch_counting_test
            ${PROJECT_BINARY_DIR}/tests/windows/bin/rocprofiler-sdk-windows-dispatch-counting-test.exe)
        add_custom_command(
            OUTPUT ${_windows_dispatch_counting_test}
            COMMAND ${ROCPROFILER_WINDOWS_TEST_HIPCC}
                    ${CMAKE_CURRENT_LIST_DIR}/dispatch_counting_service_test.cpp
                    -std=c++17
                    -O2
                    --offload-arch=gfx1151
                    --rocm-device-lib-path=${ROCM_PATH}/lib/llvm/amdgcn/bitcode
                    ${_windows_workload_compat_args}
                    -I ${PROJECT_SOURCE_DIR}/source/include
                    -I ${PROJECT_BINARY_DIR}/source/include
                    -L ${PROJECT_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}
                    -lrocprofiler-sdk
                    -Xlinker /DELAYLOAD:amdhip64_7.dll
                    -Xlinker delayimp.lib
                    -o ${_windows_dispatch_counting_test}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:rocprofiler-sdk-shared-library>
                    ${PROJECT_BINARY_DIR}/tests/windows/bin
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/amdhip64_7.dll
                    ${PROJECT_BINARY_DIR}/tests/windows/bin
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/amd_comgr.dll
                    ${PROJECT_BINARY_DIR}/tests/windows/bin
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/hsa-amd-aqlprofile64.dll
                    ${PROJECT_BINARY_DIR}/tests/windows/bin
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin/rocprofiler-register.dll
                    ${PROJECT_BINARY_DIR}/tests/windows/bin
            DEPENDS
                ${CMAKE_CURRENT_LIST_DIR}/dispatch_counting_service_test.cpp
                ${_windows_workload_compat_dependencies}
                rocprofiler-sdk-shared-library
            VERBATIM)
        add_custom_target(
            rocprofiler-sdk-windows-dispatch-counting-test ALL
            DEPENDS ${_windows_dispatch_counting_test})
        add_test(
            NAME rocprofiler-sdk.windows.dispatch-counting-service
            COMMAND ${_windows_dispatch_counting_test})
        set_tests_properties(
            rocprofiler-sdk.windows.dispatch-counting-service
            PROPERTIES
                LABELS "windows;integration;gpu;counters;dispatch-counting"
                RESOURCE_LOCK windows_gpu
                TIMEOUT 60
                ENVIRONMENT
                    "ROCPROFILER_METRICS_PATH=${PROJECT_SOURCE_DIR}/source/share/rocprofiler-sdk;ROCPROFILER_WINDOWS_DISPATCH_TEST_MODE=callback"
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin")
        add_test(
            NAME rocprofiler-sdk.windows.dispatch-counting-service.buffered
            COMMAND ${_windows_dispatch_counting_test})
        set_tests_properties(
            rocprofiler-sdk.windows.dispatch-counting-service.buffered
            PROPERTIES
                LABELS "windows;integration;gpu;counters;dispatch-counting;buffered"
                RESOURCE_LOCK windows_gpu
                TIMEOUT 60
                ENVIRONMENT
                    "ROCPROFILER_METRICS_PATH=${PROJECT_SOURCE_DIR}/source/share/rocprofiler-sdk;ROCPROFILER_WINDOWS_DISPATCH_TEST_MODE=buffered"
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin")
        add_test(
            NAME rocprofiler-sdk.windows.dispatch-counting-service.concurrent
            COMMAND ${_windows_dispatch_counting_test})
        set_tests_properties(
            rocprofiler-sdk.windows.dispatch-counting-service.concurrent
            PROPERTIES
                LABELS "windows;integration;gpu;counters;dispatch-counting;concurrent"
                RESOURCE_LOCK windows_gpu
                TIMEOUT 60
                ENVIRONMENT
                    "ROCPROFILER_METRICS_PATH=${PROJECT_SOURCE_DIR}/source/share/rocprofiler-sdk;ROCPROFILER_WINDOWS_DISPATCH_TEST_MODE=concurrent"
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin")
        add_test(
            NAME rocprofiler-sdk.windows.dispatch-counting-service.grouped
            COMMAND ${_windows_dispatch_counting_test})
        set_tests_properties(
            rocprofiler-sdk.windows.dispatch-counting-service.grouped
            PROPERTIES
                LABELS "windows;integration;gpu;counters;dispatch-counting;grouped"
                RESOURCE_LOCK windows_gpu
                TIMEOUT 60
                ENVIRONMENT
                    "ROCPROFILER_METRICS_PATH=${PROJECT_SOURCE_DIR}/source/share/rocprofiler-sdk;ROCPROFILER_WINDOWS_DISPATCH_TEST_MODE=grouped"
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}/bin")

    set(_windows_integration_root
        ${PROJECT_BINARY_DIR}/validation/windows-integration)
    set(_windows_integration_driver
        ${CMAKE_CURRENT_LIST_DIR}/rocprofv3/windows_integration.py)
    set(_windows_integration_validator
        ${CMAKE_CURRENT_LIST_DIR}/rocprofv3/validate_windows_integration.py)
    set(_windows_hsa_barrier_probe
        ${CMAKE_CURRENT_LIST_DIR}/rocprofv3/hsa_barrier_probe.py)
    set(_windows_test_python
        ${ROCPROFILER_WINDOWS_TEST_VENV_ROOT}/Scripts/python.exe)
    set(_windows_integration_common
        --python ${_windows_test_python}
        --rocprofv3 ${PROJECT_BINARY_DIR}/bin/rocprofv3
        --rocprofv3-avail ${PROJECT_BINARY_DIR}/bin/rocprofv3-avail
        --runtime-root ${ROCPROFILER_WINDOWS_TEST_RUNTIME_ROOT}
        --workload ${_windows_workload}
        --roctx-workload $<TARGET_FILE:rocprofiler-sdk-windows-roctx-workload>
        --hsa-barrier-probe ${_windows_hsa_barrier_probe}
        --sdk-build ${PROJECT_BINARY_DIR}
        --venv ${ROCPROFILER_WINDOWS_TEST_VENV_ROOT}
        --repository-root ${PROJECT_SOURCE_DIR}/../..)

    add_test(
        NAME rocprofiler-sdk.windows.installed-prefix.hip
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/validate_windows_install.py
            --prefix ${ROCPROFILER_WINDOWS_TEST_INSTALL_PREFIX}
            --output ${_windows_install_validation_root}-hip
            --case-name installed-prefix-hip
            --fresh
            --hip-workload ${_windows_workload})
    set_tests_properties(
        rocprofiler-sdk.windows.installed-prefix.hip
        PROPERTIES
            LABELS "windows;integration;gpu;install;rocprofv3;hip"
            RESOURCE_LOCK windows_gpu
            TIMEOUT 60
            ENVIRONMENT
                "PYTHONPATH=${CMAKE_CURRENT_LIST_DIR}/common;PYTHONDONTWRITEBYTECODE=1")

    foreach(_case
            availability
            baseline
            kernel-trace
            hip-trace
            hip-graph
            hip-marker
            roctx-trace
            no-overwrite
            hsa-barrier)
        string(REPLACE "-" "_" _fixture_case ${_case})
        set(_case_directory ${_windows_integration_root}/${_case})
        set(_execute_test rocprofiler-sdk.windows.${_case}.execute)
        set(_validate_test rocprofiler-sdk.windows.${_case}.validate)
        set(_fixture windows_${_fixture_case})

        add_test(
            NAME ${_execute_test}
            COMMAND ${_windows_test_python}
                    ${_windows_integration_driver}
                    --case ${_case}
                    ${_windows_integration_common}
                    --output ${_case_directory})
        if(_case STREQUAL "roctx-trace")
            set(_case_labels "windows;integration;cpu-only;safe;roctx")
        else()
            set(_case_labels "windows;integration;gpu;safe")
        endif()
        set_tests_properties(
            ${_execute_test}
            PROPERTIES
                LABELS "${_case_labels};execute"
                FIXTURES_SETUP ${_fixture}
                TIMEOUT 120)
        if(NOT _case STREQUAL "roctx-trace")
            set_tests_properties(${_execute_test} PROPERTIES RESOURCE_LOCK windows_gpu)
        endif()

        add_test(
            NAME ${_validate_test}
            COMMAND ${_windows_test_python} ${_windows_pytest_runner}
                    --owner-root ${PROJECT_BINARY_DIR}
                    --basetemp ${PROJECT_BINARY_DIR}/pytest-windows-integration-${_case}
                    -- ${_windows_integration_validator})
        set_tests_properties(
            ${_validate_test}
            PROPERTIES
                LABELS "${_case_labels};validate"
                FIXTURES_REQUIRED ${_fixture}
                TIMEOUT 30
                ENVIRONMENT
                    "ROCPROFILER_WINDOWS_INTEGRATION_CASE=${_case};ROCPROFILER_WINDOWS_INTEGRATION_RESULT=${_case_directory}/result.json;PYTHONDONTWRITEBYTECODE=1")
    endforeach()
endif()
