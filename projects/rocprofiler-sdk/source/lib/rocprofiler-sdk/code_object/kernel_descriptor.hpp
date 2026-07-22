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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace rocprofiler
{
namespace code_object
{
struct kernel_descriptor_t
{
    uint8_t  reserved0[16] = {};
    int64_t  kernel_code_entry_byte_offset = 0;
    uint8_t  reserved1[20] = {};
    uint32_t compute_pgm_rsrc3      = 0;
    uint32_t compute_pgm_rsrc1      = 0;
    uint32_t compute_pgm_rsrc2      = 0;
    uint16_t kernel_code_properties = 0;
    uint8_t  reserved2[6]           = {};
};

static_assert(sizeof(kernel_descriptor_t) == 64, "AMD kernel descriptor must be 64 bytes");
static_assert(offsetof(kernel_descriptor_t, kernel_code_entry_byte_offset) == 16);
static_assert(offsetof(kernel_descriptor_t, compute_pgm_rsrc3) == 44);
static_assert(offsetof(kernel_descriptor_t, compute_pgm_rsrc1) == 48);
static_assert(offsetof(kernel_descriptor_t, compute_pgm_rsrc2) == 52);
static_assert(offsetof(kernel_descriptor_t, kernel_code_properties) == 56);

struct kernel_descriptor_data
{
    uint32_t arch_vgpr_count  = 0;
    uint32_t accum_vgpr_count = 0;
    uint32_t sgpr_count       = 0;
    int64_t  kernel_code_entry_byte_offset = 0;
};

namespace descriptor_detail
{
constexpr uint32_t
bits(uint32_t value, uint32_t shift, uint32_t width)
{
    return (value >> shift) & ((uint32_t{1} << width) - 1);
}

inline std::optional<uint32_t>
parse_gfxip(std::string_view architecture)
{
    if(architecture.size() < 4 || architecture.substr(0, 3) != "gfx") return std::nullopt;

    auto value     = uint32_t{0};
    auto digit_cnt = size_t{0};
    for(auto index = size_t{3}; index < architecture.size(); ++index)
    {
        const auto ch = architecture[index];
        if(ch < '0' || ch > '9') break;
        value = (value * 10) + static_cast<uint32_t>(ch - '0');
        ++digit_cnt;
    }
    if(digit_cnt == 0) return std::nullopt;
    return value;
}

inline bool
has_accum_vgprs(std::string_view architecture)
{
    return architecture == "gfx908" || architecture == "gfx90a" ||
           architecture.substr(0, 5) == "gfx94" || architecture.substr(0, 5) == "gfx95";
}
}  // namespace descriptor_detail

inline std::optional<kernel_descriptor_data>
decode_kernel_descriptor(std::string_view architecture, const kernel_descriptor_t& descriptor)
{
    const auto gfxip = descriptor_detail::parse_gfxip(architecture);
    if(!gfxip) return std::nullopt;

    constexpr auto wave32_property_shift = uint32_t{10};
    const auto wave32 = descriptor_detail::bits(
                            descriptor.kernel_code_properties, wave32_property_shift, 1) != 0;

    auto output = kernel_descriptor_data{};
    output.kernel_code_entry_byte_offset = descriptor.kernel_code_entry_byte_offset;

    if(architecture == "gfx90a" || architecture.substr(0, 5) == "gfx94" ||
       architecture.substr(0, 5) == "gfx95")
    {
        output.arch_vgpr_count =
            (descriptor_detail::bits(descriptor.compute_pgm_rsrc3, 0, 5) + 1) * 4;
    }
    else
    {
        output.arch_vgpr_count =
            (descriptor_detail::bits(descriptor.compute_pgm_rsrc1, 0, 6) + 1) *
            (wave32 ? 8 : 4);
    }

    if(architecture == "gfx908")
    {
        output.accum_vgpr_count = output.arch_vgpr_count;
    }
    else if(descriptor_detail::has_accum_vgprs(architecture))
    {
        const auto combined =
            (descriptor_detail::bits(descriptor.compute_pgm_rsrc1, 0, 6) + 1) * 8;
        if(combined < output.arch_vgpr_count) return std::nullopt;
        output.accum_vgpr_count = combined - output.arch_vgpr_count;
    }

    if(*gfxip >= 1000)
    {
        // GFX10 and later always allocate 128 SGPRs per wave.
        output.sgpr_count = 128;
    }
    else
    {
        output.sgpr_count =
            (descriptor_detail::bits(descriptor.compute_pgm_rsrc1, 6, 4) / 2 + 1) * 16;
    }

    return output;
}

inline std::optional<uint64_t>
kernel_address(uint64_t kernel_object, int64_t entry_offset)
{
    if(entry_offset >= 0)
    {
        const auto offset = static_cast<uint64_t>(entry_offset);
        if(offset > std::numeric_limits<uint64_t>::max() - kernel_object) return std::nullopt;
        return kernel_object + offset;
    }

    const auto magnitude = uint64_t{0} - static_cast<uint64_t>(entry_offset);
    if(magnitude > kernel_object) return std::nullopt;
    return kernel_object - magnitude;
}
}  // namespace code_object
}  // namespace rocprofiler
