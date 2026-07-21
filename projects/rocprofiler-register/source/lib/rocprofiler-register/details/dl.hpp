// MIT License
//
// Copyright (c) 2022 Advanced Micro Devices, Inc. All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#    include <dlfcn.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif

namespace rocprofiler_register
{
namespace binary
{
using open_modes_vec_t = std::vector<int>;
using library_handle_t = void*;

#if defined(_WIN32)
using process_id_t = uint32_t;
constexpr int open_mode_lazy    = 0x01;
constexpr int open_mode_no_load = 0x02;
constexpr int open_mode_global  = 0x04;
#else
using process_id_t = pid_t;
constexpr int open_mode_lazy    = RTLD_LAZY;
constexpr int open_mode_no_load = RTLD_NOLOAD;
constexpr int open_mode_global  = RTLD_GLOBAL;
#endif

struct address_range
{
    uintptr_t start = 0;
    uintptr_t last  = 0;
};

struct segment_address_ranges
{
    std::string                filepath = {};
    std::vector<address_range> ranges   = {};
};

process_id_t
current_process_id();

std::vector<segment_address_ranges>
get_segment_addresses(process_id_t pid = current_process_id());

library_handle_t
open_library(std::string_view name, int mode);

void*
get_symbol(library_handle_t handle, std::string_view symbol);

void*
get_default_symbol(std::string_view symbol);

void
close_library(library_handle_t handle);

std::optional<std::string>
get_library_path(library_handle_t handle);

// helper function for translating generic lib name to resolved path
std::optional<std::string>
get_linked_path(std::string_view, open_modes_vec_t&& = {});
}  // namespace binary
}  // namespace rocprofiler_register
