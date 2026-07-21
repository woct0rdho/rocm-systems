// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#pragma once

#include <hsa/hsa_api_trace.h>

namespace rocprofiler
{
namespace hsa
{
namespace windows
{
void
install_internal_tables(const CoreApiTable& core_table, const AmdExtTable& ext_table);
}  // namespace windows
}  // namespace hsa
}  // namespace rocprofiler
