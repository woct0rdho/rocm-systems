add_library(rocprofiler-register-windows-profiler SHARED profiler.cpp)
target_include_directories(
    rocprofiler-register-windows-profiler PRIVATE ${CMAKE_CURRENT_LIST_DIR})
set_target_properties(
    rocprofiler-register-windows-profiler PROPERTIES OUTPUT_NAME rocprofiler-sdk)

add_library(rocprofiler-register-windows-producer SHARED producer.cpp)
target_include_directories(
    rocprofiler-register-windows-producer PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(
    rocprofiler-register-windows-producer
    PRIVATE rocprofiler-register::rocprofiler-register)
set_target_properties(
    rocprofiler-register-windows-producer PROPERTIES OUTPUT_NAME amdhip64_7)

add_executable(rocprofiler-register-windows-test registration_test.cpp)
target_include_directories(
    rocprofiler-register-windows-test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(
    rocprofiler-register-windows-test PRIVATE rocprofiler-register::headers)

foreach(_mode normal secure late invalid)
    add_test(
        NAME rocprofiler-register.windows.${_mode}
        COMMAND
            $<TARGET_FILE:rocprofiler-register-windows-test>
            $<TARGET_FILE:rocprofiler-register-windows-profiler>
            $<TARGET_FILE:rocprofiler-register-windows-producer>
            $<TARGET_FILE:rocprofiler-register>
            ${_mode})
    set_tests_properties(
        rocprofiler-register.windows.${_mode}
        PROPERTIES
            LABELS "windows;cpu-only;registration"
            TIMEOUT 30
            PASS_REGULAR_EXPRESSION "windows_registration_mode=${_mode} passed")
endforeach()
