// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include "lib/rocprofiler-sdk/counters/ioctl.hpp"

#include "lib/common/utility.hpp"

namespace rocprofiler
{
namespace counters
{
unsigned long
get_profiler_ioctl_request_for_version(uint32_t, uint32_t)
{
    return 0;
}

bool
counter_collection_has_device_lock()
{
    return false;
}

rocprofiler_status_t
counter_collection_device_lock(const rocprofiler_agent_t* agent, bool all_queues)
{
    common::consume_args(agent, all_queues);
    return ROCPROFILER_STATUS_ERROR_NOT_AVAILABLE;
}

rocprofiler_status_t
counter_collection_device_unlock(const rocprofiler_agent_t* agent)
{
    common::consume_args(agent);
    return ROCPROFILER_STATUS_ERROR_NOT_AVAILABLE;
}

bool
ptl_control_supported()
{
    return false;
}

bool
use_device_lock_at_start()
{
    return false;
}

rocprofiler_status_t
counter_collection_ptl_disable(const rocprofiler_agent_t* agent)
{
    common::consume_args(agent);
    return ROCPROFILER_STATUS_SUCCESS;
}

rocprofiler_status_t
counter_collection_ptl_enable(const rocprofiler_agent_t* agent)
{
    common::consume_args(agent);
    return ROCPROFILER_STATUS_SUCCESS;
}
}  // namespace counters
}  // namespace rocprofiler
