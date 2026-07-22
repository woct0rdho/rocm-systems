// MIT License
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include "lib/rocprofiler-sdk/code_object/kernel_descriptor_cases.hpp"

#include <iostream>
#include <string_view>

namespace
{
bool
expect(bool condition, std::string_view test_case, std::string_view field)
{
    if(condition) return true;
    std::cerr << test_case << ": " << field << " mismatch\n";
    return false;
}
}  // namespace

int
main()
{
    using namespace rocprofiler::code_object;

    auto valid = true;
    for(const auto& test_case : kernel_descriptor_cases())
    {
        const auto decoded = decode_kernel_descriptor(test_case.architecture, test_case.descriptor);
        valid &= expect(decoded.has_value() == test_case.valid, test_case.name, "decode result");
        if(!decoded) continue;

        valid &= expect(decoded->arch_vgpr_count == test_case.expected.arch_vgpr_count,
                        test_case.name,
                        "architecture VGPR count");
        valid &= expect(decoded->accum_vgpr_count == test_case.expected.accum_vgpr_count,
                        test_case.name,
                        "accumulated VGPR count");
        valid &= expect(decoded->sgpr_count == test_case.expected.sgpr_count,
                        test_case.name,
                        "SGPR count");
        valid &= expect(decoded->kernel_code_entry_byte_offset ==
                            test_case.expected.kernel_code_entry_byte_offset,
                        test_case.name,
                        "kernel entry offset");
    }

    for(const auto& test_case : kernel_address_cases())
    {
        const auto address = kernel_address(test_case.kernel_object, test_case.entry_offset);
        valid &= expect(address == test_case.expected, test_case.name, "kernel address");
    }

    return valid ? 0 : 1;
}
