// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include "lib/rocprofiler-sdk/context/context.hpp"
#include "lib/rocprofiler-sdk/context/domain.hpp"
#include "lib/rocprofiler-sdk/registration.hpp"

#include <rocprofiler-sdk/callback_tracing.h>

extern "C" rocprofiler_status_t
rocprofiler_configure_callback_tracing_service(
    rocprofiler_context_id_t               context_id,
    rocprofiler_callback_tracing_kind_t    kind,
    const rocprofiler_tracing_operation_t* operations,
    size_t                                 operations_count,
    rocprofiler_callback_tracing_cb_t      callback,
    void*                                  callback_args)
{
    if(rocprofiler::registration::get_init_status() > -1)
        return ROCPROFILER_STATUS_ERROR_CONFIGURATION_LOCKED;
    if(kind != ROCPROFILER_CALLBACK_TRACING_MARKER_CONTROL_API)
        return ROCPROFILER_STATUS_ERROR_NOT_IMPLEMENTED;
    if(!callback || (operations_count > 0 && !operations))
        return ROCPROFILER_STATUS_ERROR_INVALID_ARGUMENT;

    auto* context = rocprofiler::context::get_mutable_registered_context(context_id);
    if(!context) return ROCPROFILER_STATUS_ERROR_CONTEXT_NOT_FOUND;
    if(!context->callback_tracer)
        context->callback_tracer =
            std::make_unique<rocprofiler::context::callback_tracing_service>();

    auto& service = *context->callback_tracer;
    if(service.callback_data.at(kind).callback)
        return ROCPROFILER_STATUS_ERROR_SERVICE_ALREADY_CONFIGURED;

    auto status = rocprofiler::context::add_domain(service.domains, kind);
    if(status != ROCPROFILER_STATUS_SUCCESS) return status;
    for(size_t index = 0; index < operations_count; ++index)
    {
        status = rocprofiler::context::add_domain_op(
            service.domains, kind, operations[index]);
        if(status != ROCPROFILER_STATUS_SUCCESS) return status;
    }

    service.callback_data.at(kind) = {callback, callback_args};
    return ROCPROFILER_STATUS_SUCCESS;
}
