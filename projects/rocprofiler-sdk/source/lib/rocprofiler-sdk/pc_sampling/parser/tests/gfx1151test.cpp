// MIT License
//
// Copyright (c) 2023-2025 Advanced Micro Devices, Inc. All rights reserved.
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
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifdef NDEBUG
#    undef NDEBUG
#endif

#include "lib/rocprofiler-sdk/pc_sampling/parser/pc_record_interface.hpp"
#include "lib/rocprofiler-sdk/pc_sampling/parser/tests/gfxtest.hpp"
#include "lib/rocprofiler-sdk/pc_sampling/parser/tests/mocks.hpp"

#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace
{
using stochastic_record_t = rocprofiler_pc_sampling_record_stochastic_v0_t;

constexpr uint64_t code_object_base = 0x11510000;
constexpr uint64_t code_object_size = 0x1000;
constexpr uint64_t code_object_id   = 0x1151;

class ScopedCodeobjRange
{
public:
    ScopedCodeobjRange()
    : range{code_object_base, code_object_size, code_object_id}
    {
        rocprofiler::pc_sampling::code_object::CodeobjTableTranslatorSynchronized::Get()->insert(
            range);
    }

    ~ScopedCodeobjRange()
    {
        auto* table = rocprofiler::pc_sampling::code_object::CodeobjTableTranslatorSynchronized::Get();
        table->remove(range);
        table->clear_backlog();
    }

private:
    rocprofiler::sdk::codeobj::segment::address_range_t range;
};

uint32_t
make_perf_snapshot_data(bool     valid,
                        bool     issued,
                        uint32_t inst_type,
                        uint32_t reason,
                        bool     sampling_lock_error = false)
{
    return static_cast<uint32_t>(valid) | (static_cast<uint32_t>(issued) << 1) |
           ((inst_type & 0xF) << 2) | ((reason & 0x7) << 6) |
           (static_cast<uint32_t>(sampling_lock_error) << 14);
}

uint32_t
make_perf_snapshot_data1(uint32_t wave_count, uint32_t arb_issue, uint32_t arb_stall)
{
    return (wave_count & 0x3F) | ((arb_issue & 0xFF) << 9) | ((arb_stall & 0xFF) << 17);
}

}  // namespace

template <typename PcSamplingRecordT>
class Gfx1151HwIdTest : public WaveSnapTest<GFX1151, PcSamplingRecordT>
{
    union gfx1151_hw_id_t
    {
        uint32_t raw;
        struct
        {
            uint32_t wave_id          : 5;  ///< wave_id[4:0]
            uint32_t reserved0        : 3;
            uint32_t simd_id          : 2;  ///< simd_id[9:8]
            uint32_t cu_or_wgp_id     : 4;  ///< wgp_id[13:10]
            uint32_t reserved1        : 2;
            uint32_t shader_array_id  : 1;  ///< sa_id[16]
            uint32_t reserved2        : 1;
            uint32_t shader_engine_id : 3;  ///< se_id[20:18]
            uint32_t reserved3        : 11;
        };
    };

    void FillBuffers() override
    {
        gfx1151_hw_id_t hw_id_val0{};
        hw_id_val0.wave_id          = 0;
        hw_id_val0.simd_id          = 0;
        hw_id_val0.cu_or_wgp_id     = 0;
        hw_id_val0.shader_array_id  = 0;
        hw_id_val0.shader_engine_id = 0;

        gfx1151_hw_id_t hw_id_val1{};
        hw_id_val1.wave_id          = 15;
        hw_id_val1.simd_id          = 3;
        hw_id_val1.cu_or_wgp_id     = 15;
        hw_id_val1.shader_array_id  = 1;
        hw_id_val1.shader_engine_id = 2;

        gfx1151_hw_id_t hw_id_val2{};
        hw_id_val2.wave_id          = 7;
        hw_id_val2.simd_id          = 2;
        hw_id_val2.cu_or_wgp_id     = 6;
        hw_id_val2.shader_array_id  = 0;
        hw_id_val2.shader_engine_id = 3;

        this->buffer->genUpcomingSamples(3);
        genPCSample(hw_id_val0);
        genPCSample(hw_id_val1);
        genPCSample(hw_id_val2);
    }

    void CheckBuffers() override
    {
        auto parsed = this->buffer->get_parsed_buffer(GFX1151::gfx_ip_major,
                                                      GFX1151::gfx_ip_minor);
        ASSERT_EQ(parsed.size(), 1);
        ASSERT_EQ(parsed[0].size(), 3);
        ASSERT_EQ(compare.size(), 3);

        for(size_t i = 0; i < 3; i++)
        {
            EXPECT_EQ(compare[i].hw_id.wave_id, parsed[0][i].hw_id.wave_id);
            EXPECT_EQ(compare[i].hw_id.simd_id, parsed[0][i].hw_id.simd_id);
            EXPECT_EQ(compare[i].hw_id.cu_or_wgp_id, parsed[0][i].hw_id.cu_or_wgp_id);
            EXPECT_EQ(compare[i].hw_id.shader_array_id, parsed[0][i].hw_id.shader_array_id);
            EXPECT_EQ(compare[i].hw_id.shader_engine_id, parsed[0][i].hw_id.shader_engine_id);
        }
    }

    void genPCSample(gfx1151_hw_id_t hw_id)
    {
        PcSamplingRecordT sample;
        ::memset(&sample, 0, sizeof(sample));

        sample.hw_id.wave_id          = hw_id.wave_id;
        sample.hw_id.simd_id          = hw_id.simd_id;
        sample.hw_id.cu_or_wgp_id     = hw_id.cu_or_wgp_id;
        sample.hw_id.shader_array_id  = hw_id.shader_array_id;
        sample.hw_id.shader_engine_id = hw_id.shader_engine_id;

        compare.push_back(sample);

        perf_sample_snapshot_v1 snap;
        ::memset(&snap, 0, sizeof(snap));

        // raw register value
        snap.hw_id          = hw_id.raw;
        snap.correlation_id = this->dispatch->getMockId().raw;
        snap.perf_snapshot_data |= 0x1;  // sample is valid

        EXPECT_NE(this->dispatch.get(), nullptr);
        this->dispatch->submit(snap);
    };

    std::vector<PcSamplingRecordT> compare;
};

class Gfx1151StochasticMetadataTest
: public WaveSnapTest<GFX1151, rocprofiler_pc_sampling_record_stochastic_v0_t>
{
    struct Expected
    {
        bool     valid       = false;
        bool     wave_issued = false;
        uint32_t wave_count  = 0;
        uint32_t reason      = 0;
        uint32_t inst_type   = 0;
        uint32_t arb_issue   = 0;
        uint32_t arb_stall   = 0;
        bool     lock_error  = false;
        uint64_t pc_offset   = 0;
    };

    void FillBuffers() override
    {
        this->buffer->genUpcomingSamples(3);
        genPCSample(false, false, 0, GFX1151::TYPE_NO_INST, 0, 0, 0, false,
                    code_object_base + 0x00);
        genPCSample(true, true, 10, GFX1151::TYPE_BRANCH_NOT_TAKEN,
                    GFX1151::REASON_ARBITER_NOT_WIN, 0x41, 0x22, true, code_object_base + 0x10);
        genPCSample(true, false, 42, GFX1151::TYPE_NO_INST, GFX1151::REASON_WAITCNT, 0x18,
                    0x44, false, code_object_base + 0x20);
    }

    void CheckBuffers() override
    {
        ScopedCodeobjRange codeobj_range;
        auto parsed = this->buffer->get_parsed_buffer(GFX1151::gfx_ip_major,
                                                      GFX1151::gfx_ip_minor);
        ASSERT_EQ(parsed.size(), 1);
        ASSERT_EQ(parsed[0].size(), expected.size());

        for(size_t i = 0; i < expected.size(); ++i)
        {
            const auto& expected_sample = expected[i];
            const auto& parsed_sample   = parsed[0][i];

            if(!expected_sample.valid)
            {
                EXPECT_EQ(parsed_sample.size, 0);
                continue;
            }

            EXPECT_EQ(parsed_sample.size, sizeof(stochastic_record_t));
            EXPECT_EQ(parsed_sample.wave_issued, expected_sample.wave_issued);
            EXPECT_EQ(parsed_sample.inst_type, expected_sample.inst_type);
            EXPECT_EQ(parsed_sample.snapshot.reason_not_issued, expected_sample.reason);
            EXPECT_EQ(parsed_sample.wave_count, expected_sample.wave_count);
            EXPECT_EQ(parsed_sample.flags.has_memory_counter, 0);
            EXPECT_EQ(parsed_sample.snapshot.sampling_lock_error, expected_sample.lock_error);
            expectArbState(parsed_sample, expected_sample.arb_issue, expected_sample.arb_stall);
            EXPECT_EQ(parsed_sample.pc.code_object_id, code_object_id);
            EXPECT_EQ(parsed_sample.pc.code_object_offset, expected_sample.pc_offset);
        }
    }

    void expectArbState(const stochastic_record_t& parsed_sample,
                        uint32_t                   arb_issue,
                        uint32_t                   arb_stall)
    {
        EXPECT_EQ(parsed_sample.snapshot.arb_state_issue_brmsg, (arb_issue >> 0) & 0x1);
        EXPECT_EQ(parsed_sample.snapshot.arb_state_issue_exp, (arb_issue >> 1) & 0x1);
        EXPECT_EQ(parsed_sample.snapshot.arb_state_issue_lds_direct, (arb_issue >> 2) & 0x1);
        EXPECT_EQ(parsed_sample.snapshot.arb_state_issue_lds, (arb_issue >> 3) & 0x1);
        EXPECT_EQ(parsed_sample.snapshot.arb_state_issue_vmem_tex, (arb_issue >> 4) & 0x1);
        EXPECT_EQ(parsed_sample.snapshot.arb_state_issue_scalar, (arb_issue >> 5) & 0x1);
        EXPECT_EQ(parsed_sample.snapshot.arb_state_issue_valu, (arb_issue >> 6) & 0x1);

        EXPECT_EQ(parsed_sample.snapshot.arb_state_stall_brmsg, (arb_stall >> 0) & 0x1);
        EXPECT_EQ(parsed_sample.snapshot.arb_state_stall_exp, (arb_stall >> 1) & 0x1);
        EXPECT_EQ(parsed_sample.snapshot.arb_state_stall_lds_direct, (arb_stall >> 2) & 0x1);
        EXPECT_EQ(parsed_sample.snapshot.arb_state_stall_lds, (arb_stall >> 3) & 0x1);
        EXPECT_EQ(parsed_sample.snapshot.arb_state_stall_vmem_tex, (arb_stall >> 4) & 0x1);
        EXPECT_EQ(parsed_sample.snapshot.arb_state_stall_scalar, (arb_stall >> 5) & 0x1);
        EXPECT_EQ(parsed_sample.snapshot.arb_state_stall_valu, (arb_stall >> 6) & 0x1);
    }

    void genPCSample(bool     valid,
                     bool     issued,
                     uint32_t wave_count,
                     uint32_t inst_type,
                     uint32_t reason,
                     uint32_t arb_issue,
                     uint32_t arb_stall,
                     bool     lock_error,
                     uint64_t pc)
    {
        const auto perf_snapshot_data =
            make_perf_snapshot_data(valid, issued, inst_type, reason, lock_error);
        const auto perf_snapshot_data1 = make_perf_snapshot_data1(wave_count, arb_issue, arb_stall);
        expected.push_back(Expected{valid,
                                     issued,
                                     wave_count & 0x3F,
                                     static_cast<uint32_t>(translate_reason<GFX1151>(
                                         (perf_snapshot_data >> 6) & 0x7)),
                                     issued
                                         ? static_cast<uint32_t>(translate_inst<GFX1151>(inst_type))
                                         : ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_NO_INST,
                                     arb_issue,
                                     arb_stall,
                                     lock_error,
                                     pc - code_object_base});

        perf_sample_snapshot_v1 snap;
        ::memset(&snap, 0, sizeof(snap));
        snap.pc                  = pc;
        snap.perf_snapshot_data  = perf_snapshot_data;
        snap.perf_snapshot_data1 = perf_snapshot_data1;
        snap.correlation_id      = this->dispatch->getMockId().raw;

        EXPECT_NE(this->dispatch.get(), nullptr);
        this->dispatch->submit(snap);
    }

    std::vector<Expected> expected;
};

TEST(pcs_parser, gfx1151_parser_test)
{
    Gfx1151HwIdTest<rocprofiler_pc_sampling_record_host_trap_v0_t>{}.Test();
    Gfx1151HwIdTest<rocprofiler_pc_sampling_record_stochastic_v0_t>{}.Test();
    WaveOtherFieldsTest<GFX1151, rocprofiler_pc_sampling_record_stochastic_v0_t>{}.Test();
    WaveOtherFieldsTest<GFX1151, rocprofiler_pc_sampling_record_host_trap_v0_t>{}.Test();

    MidMacroPCCorrection<GFX1151, rocprofiler_pc_sampling_record_host_trap_v0_t>{}.Test();
    MidMacroPCCorrection<GFX1151, rocprofiler_pc_sampling_record_stochastic_v0_t>{}.Test();

    Gfx1151StochasticMetadataTest{}.Test();
}

TEST(pcs_parser, gfx1151_instruction_classification)
{
    struct TestCase
    {
        std::string_view instruction;
        uint32_t         expected;
    };

    const std::array<TestCase, 17> test_cases = {{
        {"v_mfma_f32_16x16x16f16 v[0:3], v0, v1, v[0:3]",
         ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_MATRIX},
        {"v_add_f32 v0, v1, v2", ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_VALU},
        {"global_load_dword v0, v[0:1], off", ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_FLAT},
        {"flat_store_dword v[0:1], v2", ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_FLAT},
        {"buffer_load_dword v0, off, s[0:3], s4", ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_TEX},
        {"image_sample v[0:3], v[4:7], s[0:7], s[8:11]",
         ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_TEX},
        {"tbuffer_load_format_x v0, off, s[0:3], dfmt:1, nfmt:1",
         ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_TEX},
        {"ds_read_b32 v0, v1", ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_LDS},
        {"exp mrt0 v0, v1, v2, v3", ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_EXPORT},
        {"s_sendmsg sendmsg(MSG_DEALLOC_VGPRS)",
         ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_MESSAGE},
        {"s_barrier", ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_BARRIER},
        {"s_branch label", ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_BRANCH_TAKEN},
        {"s_cbranch_scc0 label", ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_BRANCH_TAKEN},
        {"s_setpc_b64 s[0:1]", ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_JUMP},
        {"s_swappc_b64 s[0:1], s[2:3]", ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_JUMP},
        {"s_add_u32 s0, s1, s2", ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_SCALAR},
        {"unknown_op v0, v1", ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_OTHER},
    }};

    for(const auto& test_case : test_cases)
        EXPECT_EQ(classify_gfx1151_instruction_type(test_case.instruction), test_case.expected)
            << test_case.instruction;
}

TEST(pcs_parser, gfx1151_decoded_instruction_post_process)
{
    stochastic_record_t issued{};
    issued.wave_issued = true;
    apply_gfx1151_decoded_instruction(issued, "v_mfma_f32_16x16x16f16 v[0:3], v0, v1, v[0:3]");
    EXPECT_EQ(issued.wave_issued, true);
    EXPECT_EQ(issued.inst_type, ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_MATRIX);

    stochastic_record_t waitcnt{};
    waitcnt.wave_issued                = true;
    waitcnt.inst_type                  = ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_OTHER;
    waitcnt.snapshot.reason_not_issued =
        ROCPROFILER_PC_SAMPLING_INSTRUCTION_NOT_ISSUED_REASON_ALU_DEPENDENCY;

    apply_gfx1151_decoded_instruction(waitcnt, "s_waitcnt vmcnt(0)");
    EXPECT_EQ(waitcnt.wave_issued, false);
    EXPECT_EQ(waitcnt.inst_type, ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_NO_INST);
    EXPECT_EQ(waitcnt.snapshot.reason_not_issued,
              ROCPROFILER_PC_SAMPLING_INSTRUCTION_NOT_ISSUED_REASON_WAITCNT);

    stochastic_record_t stalled{};
    stalled.wave_issued = false;
    stalled.inst_type   = ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_NO_INST;
    apply_gfx1151_decoded_instruction(stalled, "v_add_f32 v0, v1, v2");
    EXPECT_EQ(stalled.wave_issued, false);
    EXPECT_EQ(stalled.inst_type, ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_NO_INST);

    stochastic_record_t branch_not_taken{};
    branch_not_taken.wave_issued = true;
    branch_not_taken.inst_type   = ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_BRANCH_NOT_TAKEN;
    apply_gfx1151_decoded_instruction(branch_not_taken, "s_cbranch_scc0 label");
    EXPECT_EQ(branch_not_taken.wave_issued, true);
    EXPECT_EQ(branch_not_taken.inst_type,
              ROCPROFILER_PC_SAMPLING_INSTRUCTION_TYPE_BRANCH_NOT_TAKEN);
}
