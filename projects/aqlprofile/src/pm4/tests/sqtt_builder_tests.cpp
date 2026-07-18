//Copyright © Advanced Micro Devices, Inc., or its affiliates.
//SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include "def/gpu_block_info.h"
#include "def/gfx11_def.h"
#include "../trace_config.h"

namespace pm4_builder {

// Minimal implementation of required types for testing
struct AgentInfo {
  uint32_t gfxip;
  uint32_t xcc_num;
  uint32_t se_num;
};

enum hsa_status_t {
  HSA_STATUS_SUCCESS = 0x0,
};

struct TraceControl
{
  uint32_t status{0};
  uint32_t cntr{0};
  uint32_t wptr{0};
  uint32_t status2{0};
  uint64_t gpu_clock_cnt_start{0};
  uint64_t gpu_clock_cnt_end{0};
  uint32_t status_double_buffer{0};
};

// Minimal primitives for testing
struct TestPrimitives {
  static constexpr uint32_t GFXIP_LEVEL = 9;
  static constexpr uint32_t TT_BUFF_ALIGN_SHIFT = 12;  // 4KB alignment
  static constexpr uint32_t TT_CONTROL_UTC_ERR_MASK = 0x1;
  static constexpr uint32_t TT_CONTROL_FULL_MASK = 0x2;
  static constexpr uint32_t TT_WRITE_PTR_MASK = 0x4;
  static constexpr uint32_t SQ_THREAD_TRACE_USERDATA_2 = 0x1000;
  static constexpr Register SQ_THREAD_TRACE_STATUS2_ADDR{};
  
  static uint32_t grbm_broadcast_value() { return 0xFFFFFFFF; }
  static uint32_t sqtt_mode_off_value() { return 0; }
  static uint32_t sqtt_mode_on_value() { return 1; }
  static uint32_t sqtt_buffer_size_value(uint64_t size, uint32_t) {
    return static_cast<uint32_t>(size >> TT_BUFF_ALIGN_SHIFT);
  }
};

struct TestGfx115xPrimitives {
  static constexpr uint32_t GFXIP_LEVEL = 11;
  static constexpr uint32_t TT_BUFF_ALIGN_SHIFT = 12;
  static constexpr uint32_t TT_CONTROL_UTC_ERR_MASK = 0x10;
  static constexpr uint32_t TT_CONTROL_FULL_MASK = 0x21;
  static constexpr uint32_t TT_WRITE_PTR_MASK = 0x3fffffff;
  static constexpr Register SQ_THREAD_TRACE_STATUS_ADDR = Register(0x1000);
  static constexpr Register SQ_THREAD_TRACE_STATUS2_ADDR = Register(0x2000);
  static constexpr Register SQ_THREAD_TRACE_CNTR_ADDR = Register(0x3000);
  static constexpr Register SQ_THREAD_TRACE_WPTR_ADDR = Register(0x4000);
  static constexpr Register GRBM_GFX_INDEX_ADDR = Register(0x5000);
  static constexpr uint32_t COPY_DATA_SEL_COUNT_1DW_PRM = 1;

  static uint32_t grbm_broadcast_value() { return 0xffffffff; }
  static uint32_t grbm_se_sh_index_value(const uint32_t& se_index, const uint32_t&) {
    return se_index;
  }
};

struct CopyRegCall {
  Register reg;
  const void* dst;
  bool wait;
};

// Minimal command buffer for testing
class CmdBuffer {
public:
  void Clear() {}
  size_t DwSize() const { return 0; }
  const void* Data() const { return nullptr; }
  void Assign(size_t, uint32_t) {}
  std::vector<uint32_t> commands;
};

// Minimal command builder for testing
class TestBuilder {
public:
  TestBuilder(const AgentInfo*) {}
  
  void BuildWriteUConfigRegPacket(CmdBuffer* cmd_buffer, uint32_t addr, uint32_t value) {
    cmd_buffer->commands.push_back(addr);
    cmd_buffer->commands.push_back(value);
  }
  
  void BuildPredExecPacket(CmdBuffer*, uint32_t, uint32_t) {}
  void BuildWriteWaitIdlePacket(CmdBuffer*) {}
  void BuildCacheFlushPacket(CmdBuffer*, size_t, size_t) {}
};

class TestGfx115xBuilder {
public:
  TestGfx115xBuilder(const AgentInfo*) { calls.clear(); }

  void BuildCopyRegDataPacket(CmdBuffer*, const Register& reg, const void* dst, uint32_t,
                              bool wait) {
    calls.push_back({reg, dst, wait});
  }

  void BuildWriteUConfigRegPacket(CmdBuffer*, const Register&, uint32_t) {}
  void BuildPredExecPacket(CmdBuffer*, uint32_t, uint32_t) {}
  void BuildWriteWaitIdlePacket(CmdBuffer*) {}
  void BuildCacheFlushPacket(CmdBuffer*, size_t, size_t) {}

  static std::vector<CopyRegCall> calls;
};

std::vector<CopyRegCall> TestGfx115xBuilder::calls;

// Actual GpuSqttBuilder implementation for testing
template <typename Builder, typename Primitives>
class GpuSqttBuilder {
public:
  explicit GpuSqttBuilder(const AgentInfo* agent_info) 
    : xcc_number_(agent_info->xcc_num)
    , se_number_total(agent_info->se_num)
    , builder_(agent_info) {}

  size_t GetUTCErrorMask() const { return Primitives::TT_CONTROL_UTC_ERR_MASK; }
  size_t GetBufferFullMask() const { return Primitives::TT_CONTROL_FULL_MASK; }
  size_t GetWritePtrMask() const { return Primitives::TT_WRITE_PTR_MASK; }
  size_t GetWritePtrBlk() const { return 32; }
  size_t BufferAlignment() const { return Primitives::TT_BUFF_ALIGN_SHIFT; }
  uint32_t GetXCCNumber() const { return xcc_number_; }

  uint64_t PopCount(uint64_t se_mask) const {
    uint64_t num_enabled = 0;
    while (se_mask) {
      num_enabled += se_mask & 1;
      se_mask >>= 1;
    }
    return std::max<uint64_t>(num_enabled, 1u);
  }

  uint64_t GetBaseStep(uint64_t buffersize, uint64_t se_mask) const {
    uint64_t num_enabled = PopCount(se_mask);
    int64_t num_disabled = (64 - num_enabled) << Primitives::TT_BUFF_ALIGN_SHIFT;
    int64_t buffer_per_se = std::max<int64_t>(0, buffersize - num_disabled) / num_enabled;
    return uint64_t(buffer_per_se) & ~((1ULL << Primitives::TT_BUFF_ALIGN_SHIFT) - 1);
  }

  void ReadValues(CmdBuffer* cmd_buffer, const TraceConfig* config, size_t se_index) {
    auto& control = reinterpret_cast<TraceControl*>(config->control_buffer_ptr)[se_index];

    builder_.BuildCopyRegDataPacket(cmd_buffer, Primitives::SQ_THREAD_TRACE_STATUS_ADDR,
                                    &control.status, Primitives::COPY_DATA_SEL_COUNT_1DW_PRM,
                                    true);
    builder_.BuildCopyRegDataPacket(cmd_buffer, Primitives::SQ_THREAD_TRACE_CNTR_ADDR,
                                    &control.cntr, Primitives::COPY_DATA_SEL_COUNT_1DW_PRM, true);
    builder_.BuildCopyRegDataPacket(cmd_buffer, Primitives::SQ_THREAD_TRACE_WPTR_ADDR,
                                    &control.wptr, Primitives::COPY_DATA_SEL_COUNT_1DW_PRM, true);
    if (!(Primitives::SQ_THREAD_TRACE_STATUS2_ADDR == Register{})) {
      builder_.BuildCopyRegDataPacket(cmd_buffer, Primitives::SQ_THREAD_TRACE_STATUS2_ADDR,
                                      &control.status2, Primitives::COPY_DATA_SEL_COUNT_1DW_PRM,
                                      true);
    }
  }

  void GetStatusPacket(CmdBuffer* cmd_buffer, TraceConfig*, TraceControl& control, int) {
    auto status_addr = (!(Primitives::SQ_THREAD_TRACE_STATUS2_ADDR == Register{}))
                           ? Primitives::SQ_THREAD_TRACE_STATUS2_ADDR
                           : Primitives::SQ_THREAD_TRACE_STATUS_ADDR;
    builder_.BuildCopyRegDataPacket(cmd_buffer, status_addr, &control.status_double_buffer,
                                    Primitives::COPY_DATA_SEL_COUNT_1DW_PRM, false);
  }

private:
  uint32_t xcc_number_;
  size_t se_number_total;
  Builder builder_;
};

class SqttBuilderTest : public ::testing::Test {
protected:
  void SetUp() override {
    agent_info.gfxip = 9;
    agent_info.xcc_num = 2;
    agent_info.se_num = 4;
  }

  AgentInfo agent_info;
  std::vector<uint8_t> data_buffer;
  std::vector<uint8_t> control_buffer;
};

TEST_F(SqttBuilderTest, DISABLED_BufferStepCalculation) {
  GpuSqttBuilder<TestBuilder, TestPrimitives> builder(&agent_info);

  // Test with different buffer sizes and SE masks
  const uint64_t total_buffer = 1024 * 1024;  // 1MB total
  
  // Test case 1: All SEs enabled (4 SEs)
  uint64_t mask1 = 0xF;  // 0b1111
  uint64_t step1 = builder.GetBaseStep(total_buffer, mask1);
  EXPECT_EQ(step1 * builder.PopCount(mask1), total_buffer);
  EXPECT_EQ(step1 & ((1ULL << TestPrimitives::TT_BUFF_ALIGN_SHIFT) - 1), 0);  // Check alignment

  // Test case 2: Half SEs enabled (2 SEs)
  uint64_t mask2 = 0x3;  // 0b0011
  uint64_t step2 = builder.GetBaseStep(total_buffer, mask2);
  EXPECT_EQ(step2 * builder.PopCount(mask2), total_buffer / 2);
  EXPECT_EQ(step2 & ((1ULL << TestPrimitives::TT_BUFF_ALIGN_SHIFT) - 1), 0);  // Check alignment
}

TEST_F(SqttBuilderTest, PopulationCount) {
  GpuSqttBuilder<TestBuilder, TestPrimitives> builder(&agent_info);

  // Test different SE mask configurations
  EXPECT_EQ(builder.PopCount(0x1), 1);    // Single SE
  EXPECT_EQ(builder.PopCount(0x3), 2);    // Two SEs
  EXPECT_EQ(builder.PopCount(0xF), 4);    // Four SEs
  EXPECT_EQ(builder.PopCount(0x0), 1);    // No SEs (minimum is 1)
  EXPECT_EQ(builder.PopCount(0x5), 2);    // Non-contiguous SEs
}

TEST_F(SqttBuilderTest, ThreadTraceStatusMasks) {
  GpuSqttBuilder<TestBuilder, TestPrimitives> builder(&agent_info);

  // Verify mask values
  EXPECT_EQ(builder.GetUTCErrorMask(), TestPrimitives::TT_CONTROL_UTC_ERR_MASK);
  EXPECT_EQ(builder.GetBufferFullMask(), TestPrimitives::TT_CONTROL_FULL_MASK);
  EXPECT_EQ(builder.GetWritePtrMask(), TestPrimitives::TT_WRITE_PTR_MASK);
  
  // Verify masks are unique
  EXPECT_NE(builder.GetUTCErrorMask(), builder.GetBufferFullMask());
  EXPECT_NE(builder.GetUTCErrorMask(), builder.GetWritePtrMask());
  EXPECT_NE(builder.GetBufferFullMask(), builder.GetWritePtrMask());
}

TEST_F(SqttBuilderTest, XCCConfiguration) {
  GpuSqttBuilder<TestBuilder, TestPrimitives> builder(&agent_info);

  // Test XCC number configuration
  EXPECT_EQ(builder.GetXCCNumber(), agent_info.xcc_num);
  
  // Test with different XCC configurations
  agent_info.xcc_num = 1;
  GpuSqttBuilder<TestBuilder, TestPrimitives> single_xcc(&agent_info);
  EXPECT_EQ(single_xcc.GetXCCNumber(), 1);

  agent_info.xcc_num = 4;
  GpuSqttBuilder<TestBuilder, TestPrimitives> multi_xcc(&agent_info);
  EXPECT_EQ(multi_xcc.GetXCCNumber(), 4);
}

TEST_F(SqttBuilderTest, BufferAlignmentAndBlockSize) {
  GpuSqttBuilder<TestBuilder, TestPrimitives> builder(&agent_info);

  // Test buffer alignment
  EXPECT_EQ(builder.BufferAlignment(), TestPrimitives::TT_BUFF_ALIGN_SHIFT);
  EXPECT_EQ(1ULL << builder.BufferAlignment(), 4096);  // 4KB alignment

  // Test write pointer block size
  EXPECT_EQ(builder.GetWritePtrBlk(), 32);  // 32-byte blocks
}

TEST_F(SqttBuilderTest, Gfx115xPrimitivesUseStatus2AndDoubleBufferBits) {
  using gfxip::gfx11::gfx115x_sqtt_prim;

  EXPECT_FALSE(gfx115x_sqtt_prim::SQ_THREAD_TRACE_STATUS2_ADDR ==
               gfx115x_sqtt_prim::SQ_THREAD_TRACE_STATUS_ADDR);
  EXPECT_EQ(gfx115x_sqtt_prim::TT_CONTROL_FULL_MASK,
            SQ_THREAD_TRACE_STATUS2__BUF0_FULL_MASK |
                SQ_THREAD_TRACE_STATUS2__BUF1_FULL_MASK |
                SQ_THREAD_TRACE_STATUS2__WRITE_BUF_FULL_MASK);
  EXPECT_EQ(gfx115x_sqtt_prim::TT_LOCKDOWN_FAIL,
            SQ_THREAD_TRACE_STATUS2__PACKET_LOST_BUF_NO_LOCKDOWN_MASK);
  EXPECT_FALSE(gfx115x_sqtt_prim::SQ_THREAD_TRACE_BUF1_BASE_LO_ADDR == Register{});
  EXPECT_FALSE(gfx115x_sqtt_prim::SQ_THREAD_TRACE_BUF1_SIZE_ADDR == Register{});
  EXPECT_TRUE(gfx115x_sqtt_prim::SQ_THREAD_TRACE_BUF1_BASE_HI_ADDR == Register{});

  EXPECT_EQ(gfx115x_sqtt_prim::sqtt_ctrl_value(true, true) &
                SQ_THREAD_TRACE_CTRL__DOUBLE_BUFFER_MASK,
            SQ_THREAD_TRACE_CTRL__DOUBLE_BUFFER_MASK);
  EXPECT_EQ(gfx115x_sqtt_prim::sqtt_ctrl_value(true, false) &
                SQ_THREAD_TRACE_CTRL__DOUBLE_BUFFER_MASK,
            0u);

  EXPECT_TRUE(gfxip::gfx11::gfx11_cntx_prim::SQ_THREAD_TRACE_STATUS2_ADDR == Register{});
  EXPECT_EQ(gfxip::gfx11::gfx11_cntx_prim::TT_CONTROL_FULL_MASK, 0u);
}

TEST_F(SqttBuilderTest, Gfx115xBuilderCopiesStatus2AndQueriesStatus2) {
  GpuSqttBuilder<TestGfx115xBuilder, TestGfx115xPrimitives> builder(&agent_info);
  TraceControl control{};
  CmdBuffer cmd_buffer;
  TraceConfig config;
  config.se_number = agent_info.se_num;
  config.xcc_number = agent_info.xcc_num;
  config.control_buffer_ptr = &control;

  TestGfx115xBuilder::calls.clear();
  builder.ReadValues(&cmd_buffer, &config, 0);
  ASSERT_EQ(TestGfx115xBuilder::calls.size(), 4u);
  EXPECT_EQ(TestGfx115xBuilder::calls[0].reg,
            TestGfx115xPrimitives::SQ_THREAD_TRACE_STATUS_ADDR);
  EXPECT_EQ(TestGfx115xBuilder::calls[0].dst, &control.status);
  EXPECT_EQ(TestGfx115xBuilder::calls[1].reg,
            TestGfx115xPrimitives::SQ_THREAD_TRACE_CNTR_ADDR);
  EXPECT_EQ(TestGfx115xBuilder::calls[2].reg,
            TestGfx115xPrimitives::SQ_THREAD_TRACE_WPTR_ADDR);
  EXPECT_EQ(TestGfx115xBuilder::calls[3].reg,
            TestGfx115xPrimitives::SQ_THREAD_TRACE_STATUS2_ADDR);
  EXPECT_EQ(TestGfx115xBuilder::calls[3].dst, &control.status2);

  TestGfx115xBuilder::calls.clear();
  builder.GetStatusPacket(&cmd_buffer, &config, control, 0);
  ASSERT_EQ(TestGfx115xBuilder::calls.size(), 1u);
  EXPECT_EQ(TestGfx115xBuilder::calls[0].reg,
            TestGfx115xPrimitives::SQ_THREAD_TRACE_STATUS2_ADDR);
  EXPECT_EQ(TestGfx115xBuilder::calls[0].dst, &control.status_double_buffer);
}

} // namespace pm4_builder
