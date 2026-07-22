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

#include "kernel_descriptor.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace rocprofiler
{
namespace code_object
{
struct kernel_descriptor_case
{
    std::string_view       name         = {};
    std::string_view       architecture = {};
    kernel_descriptor_t    descriptor   = {};
    bool                   valid        = false;
    kernel_descriptor_data expected     = {};
};

inline std::array<kernel_descriptor_case, 5>
kernel_descriptor_cases()
{
    auto output = std::array<kernel_descriptor_case, 5>{};

    output[0].name                                     = "gfx1151_wave32_registers";
    output[0].architecture                             = "gfx1151";
    output[0].descriptor.kernel_code_entry_byte_offset = 256;
    output[0].descriptor.compute_pgm_rsrc1             = 4;
    output[0].descriptor.kernel_code_properties        = uint16_t{1} << 10;
    output[0].valid                                    = true;
    output[0].expected                                 = kernel_descriptor_data{40, 0, 128, 256};

    output[1].name                         = "gfx90a_accumulated_registers";
    output[1].architecture                 = "gfx90a";
    output[1].descriptor.compute_pgm_rsrc3 = 7;
    output[1].descriptor.compute_pgm_rsrc1 = 5 | (6 << 6);
    output[1].valid                        = true;
    output[1].expected                     = kernel_descriptor_data{32, 16, 64, 0};

    output[2].name                         = "gfx908_accumulated_registers";
    output[2].architecture                 = "gfx908";
    output[2].descriptor.compute_pgm_rsrc1 = 3 | (4 << 6);
    output[2].valid                        = true;
    output[2].expected                     = kernel_descriptor_data{16, 16, 48, 0};

    output[3].name         = "unknown_architecture";
    output[3].architecture = "not-an-architecture";

    output[4].name                         = "inconsistent_accumulated_registers";
    output[4].architecture                 = "gfx90a";
    output[4].descriptor.compute_pgm_rsrc3 = 7;

    return output;
}

struct kernel_address_case
{
    std::string_view        name          = {};
    uint64_t                kernel_object = 0;
    int64_t                 entry_offset  = 0;
    std::optional<uint64_t> expected      = {};
};

inline std::array<kernel_address_case, 4>
kernel_address_cases()
{
    return {{{"positive_offset", 0x1000, 0x80, 0x1080},
             {"negative_offset", 0x1000, -0x80, 0x0f80},
             {"negative_underflow", 0x40, -0x80, std::nullopt},
             {"positive_overflow", std::numeric_limits<uint64_t>::max() - 4, 8, std::nullopt}}};
}
}  // namespace code_object
}  // namespace rocprofiler
