// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include "lib/rocprofiler-sdk/code_object/kernel_descriptor_cases.hpp"

#include <gtest/gtest.h>

#include <string>

namespace rocprofiler
{
namespace code_object
{
namespace
{
TEST(kernel_descriptor, shared_decode_cases)
{
    for(const auto& test_case : kernel_descriptor_cases())
    {
        SCOPED_TRACE(std::string{test_case.name});
        const auto decoded = decode_kernel_descriptor(test_case.architecture, test_case.descriptor);
        ASSERT_EQ(decoded.has_value(), test_case.valid);
        if(!decoded) continue;

        EXPECT_EQ(decoded->arch_vgpr_count, test_case.expected.arch_vgpr_count);
        EXPECT_EQ(decoded->accum_vgpr_count, test_case.expected.accum_vgpr_count);
        EXPECT_EQ(decoded->sgpr_count, test_case.expected.sgpr_count);
        EXPECT_EQ(decoded->kernel_code_entry_byte_offset,
                  test_case.expected.kernel_code_entry_byte_offset);
    }
}

TEST(kernel_descriptor, shared_address_cases)
{
    for(const auto& test_case : kernel_address_cases())
    {
        SCOPED_TRACE(std::string{test_case.name});
        const auto address = kernel_address(test_case.kernel_object, test_case.entry_offset);
        ASSERT_EQ(address.has_value(), test_case.expected.has_value());
        if(address) EXPECT_EQ(*address, *test_case.expected);
    }
}
}  // namespace
}  // namespace code_object
}  // namespace rocprofiler
