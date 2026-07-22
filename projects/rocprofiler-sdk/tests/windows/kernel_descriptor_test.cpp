// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include "lib/rocprofiler-sdk/code_object/kernel_descriptor.hpp"

#include <cstdint>
#include <iostream>
#include <limits>

namespace
{
bool
expect(bool condition, const char* message)
{
    if(condition) return true;
    std::cerr << message << '\n';
    return false;
}
}  // namespace

int
main()
{
    using namespace rocprofiler::code_object;

    auto gfx1151                                = kernel_descriptor_t{};
    gfx1151.kernel_code_entry_byte_offset       = 256;
    gfx1151.compute_pgm_rsrc1                    = 4;
    gfx1151.kernel_code_properties               = uint16_t{1} << 10;
    const auto gfx1151_data                      = decode_kernel_descriptor("gfx1151", gfx1151);

    auto gfx90a                   = kernel_descriptor_t{};
    gfx90a.compute_pgm_rsrc3      = 7;
    gfx90a.compute_pgm_rsrc1      = 5 | (6 << 6);
    const auto gfx90a_data        = decode_kernel_descriptor("gfx90a", gfx90a);

    auto valid = true;
    valid &= expect(gfx1151_data.has_value(), "gfx1151 descriptor did not decode");
    valid &= expect(gfx1151_data && gfx1151_data->arch_vgpr_count == 40,
                    "gfx1151 architecture VGPR count mismatch");
    valid &= expect(gfx1151_data && gfx1151_data->accum_vgpr_count == 0,
                    "gfx1151 accumulated VGPR count mismatch");
    valid &= expect(gfx1151_data && gfx1151_data->sgpr_count == 128,
                    "gfx1151 SGPR count mismatch");
    valid &= expect(gfx90a_data.has_value(), "gfx90a descriptor did not decode");
    valid &= expect(gfx90a_data && gfx90a_data->arch_vgpr_count == 32,
                    "gfx90a architecture VGPR count mismatch");
    valid &= expect(gfx90a_data && gfx90a_data->accum_vgpr_count == 16,
                    "gfx90a accumulated VGPR count mismatch");
    valid &= expect(gfx90a_data && gfx90a_data->sgpr_count == 64,
                    "gfx90a SGPR count mismatch");
    valid &= expect(!decode_kernel_descriptor("invalid", gfx1151),
                    "invalid architecture unexpectedly decoded");
    valid &= expect(kernel_address(0x1000, -0x80) == 0x0f80,
                    "negative kernel entry offset mismatch");
    valid &= expect(!kernel_address(std::numeric_limits<uint64_t>::max() - 4, 8),
                    "overflowing kernel address unexpectedly succeeded");

    return valid ? 0 : 1;
}
