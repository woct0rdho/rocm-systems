// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include "lib/rocprofiler-sdk/code_object/kernel_descriptor.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace rocprofiler
{
namespace code_object
{
namespace
{
TEST(kernel_descriptor, gfx1151_wave32_registers)
{
    auto descriptor                              = kernel_descriptor_t{};
    descriptor.kernel_code_entry_byte_offset     = 256;
    descriptor.compute_pgm_rsrc1                  = 4;
    descriptor.kernel_code_properties             = uint16_t{1} << 10;

    const auto decoded = decode_kernel_descriptor("gfx1151", descriptor);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->arch_vgpr_count, 40);
    EXPECT_EQ(decoded->accum_vgpr_count, 0);
    EXPECT_EQ(decoded->sgpr_count, 128);
    EXPECT_EQ(decoded->kernel_code_entry_byte_offset, 256);
}

TEST(kernel_descriptor, gfx90a_accumulated_registers)
{
    auto descriptor               = kernel_descriptor_t{};
    descriptor.compute_pgm_rsrc3  = 7;
    descriptor.compute_pgm_rsrc1  = 5 | (6 << 6);

    const auto decoded = decode_kernel_descriptor("gfx90a", descriptor);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->arch_vgpr_count, 32);
    EXPECT_EQ(decoded->accum_vgpr_count, 16);
    EXPECT_EQ(decoded->sgpr_count, 64);
}

TEST(kernel_descriptor, gfx908_accumulated_registers)
{
    auto descriptor              = kernel_descriptor_t{};
    descriptor.compute_pgm_rsrc1 = 3 | (4 << 6);

    const auto decoded = decode_kernel_descriptor("gfx908", descriptor);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->arch_vgpr_count, 16);
    EXPECT_EQ(decoded->accum_vgpr_count, 16);
    EXPECT_EQ(decoded->sgpr_count, 48);
}

TEST(kernel_descriptor, rejects_unknown_or_inconsistent_descriptors)
{
    auto descriptor              = kernel_descriptor_t{};
    descriptor.compute_pgm_rsrc3 = 7;

    EXPECT_FALSE(decode_kernel_descriptor("not-an-architecture", descriptor));
    EXPECT_FALSE(decode_kernel_descriptor("gfx90a", descriptor));
}

TEST(kernel_descriptor, computes_checked_entry_address)
{
    EXPECT_EQ(kernel_address(0x1000, 0x80), 0x1080);
    EXPECT_EQ(kernel_address(0x1000, -0x80), 0x0f80);
    EXPECT_FALSE(kernel_address(0x40, -0x80));
    EXPECT_FALSE(kernel_address(std::numeric_limits<uint64_t>::max() - 4, 8));
}
}  // namespace
}  // namespace code_object
}  // namespace rocprofiler
