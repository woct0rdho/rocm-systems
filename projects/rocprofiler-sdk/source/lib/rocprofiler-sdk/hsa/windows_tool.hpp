// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once

#include <hsa/hsa_api_trace.h>
#include <rocprofiler-sdk/fwd.h>

#include <cstdint>
#include <optional>
#include <string>

namespace rocprofiler
{
namespace hsa
{
namespace windows
{
struct kernel_metadata
{
    rocprofiler_kernel_id_t kernel_id{0};
    uint64_t                kernel_object             = 0;
    uint64_t                kernel_address            = 0;
    int64_t                 kernel_code_entry_offset  = 0;
    uint32_t                kernarg_segment_size      = 0;
    uint32_t                kernarg_segment_alignment = 0;
    uint32_t                group_segment_size        = 0;
    uint32_t                private_segment_size      = 0;
    uint32_t                arch_vgpr_count           = 0;
    uint32_t                accum_vgpr_count          = 0;
    uint32_t                sgpr_count                = 0;
    std::string             name                      = {};
    std::string             architecture              = {};
    std::string             error                     = {};
    bool                    valid                     = false;
};

bool
set_api_table(::HsaApiTable* api_table, uint64_t runtime_version, uint64_t failed_tool_count);

void
register_kernel_metadata(kernel_metadata metadata);

std::string
get_kernel_name(rocprofiler_kernel_id_t kernel_id);

std::optional<kernel_metadata>
get_kernel_metadata(rocprofiler_kernel_id_t kernel_id);
}  // namespace windows
}  // namespace hsa
}  // namespace rocprofiler
