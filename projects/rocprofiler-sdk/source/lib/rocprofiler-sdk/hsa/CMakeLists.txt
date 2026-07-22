#
if(ROCPROFILER_BUILD_WINDOWS_MINIMAL)
    set(ROCPROFILER_LIB_HSA_SOURCES
        agent_cache.cpp
        aql_packet.cpp
        hsa_barrier.cpp
        profile_serializer.cpp
        queue_controller.cpp
        queue_interposition_windows.cpp
        queue_windows.cpp
        signal_pool.cpp
        windows_api.cpp)
else()
    set(ROCPROFILER_LIB_HSA_SOURCES
        abi.cpp
        agent_cache.cpp
        aql_packet.cpp
        async_copy.cpp
        hsa_barrier.cpp
        hsa.cpp
        memory_allocation.cpp
        pc_sampling.cpp
        pc_sampling.hpp
        profile_serializer.cpp
        queue_controller.cpp
        queue_interposition.cpp
        queue.cpp
        scratch_memory.cpp
        signal_pool.cpp)
endif()

set(ROCPROFILER_LIB_HSA_HEADERS
    agent_cache.hpp
    aql_packet.hpp
    async_copy.hpp
    memory_allocation.hpp
    defines.hpp
    hsa_barrier.hpp
    hsa.hpp
    profile_serializer.hpp
    queue_controller.hpp
    queue_interposition.hpp
    queue.hpp
    queue_info_session.hpp
    rocprofiler_packet.hpp
    scratch_memory.hpp
    signal.hpp
    signal_pool.hpp
    utils.hpp)

target_sources(rocprofiler-sdk-object-library PRIVATE ${ROCPROFILER_LIB_HSA_SOURCES}
                                                      ${ROCPROFILER_LIB_HSA_HEADERS})

if(NOT ROCPROFILER_BUILD_WINDOWS_MINIMAL)
    add_subdirectory(details)
endif()
