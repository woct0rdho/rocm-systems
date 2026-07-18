// Copyright © Advanced Micro Devices, Inc., or its affiliates.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <climits>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>

#define private public
#include "lib/aqlprofile/core/pm4_factory.h"
#undef private
#include "lib/aqlprofile/core/gfx11_factory.h"
#include "lib/aqlprofile/def/gfx11_def.h"

using namespace aql_profile;

namespace
{
// Helper to create a valid agent info struct
aqlprofile_agent_info_v1_t
makeTestAgentInfo(const char* gfxip = "gfx900")
{
    aqlprofile_agent_info_v1_t info{};
    info.agent_gfxip          = strdup(gfxip);
    info.cu_num               = 64;
    info.se_num               = 4;
    info.xcc_num              = 1;
    info.shader_arrays_per_se = 2;
    info.domain               = 0;
    info.location_id          = 0x1234;
    return info;
}

}  // namespace

// Test: Register agent and retrieve info (happy path)
TEST(Pm4FactoryTest, RegisterAgentAndGetAgentInfo)
{
    auto                      agentInfo = makeTestAgentInfo();
    aqlprofile_agent_handle_t handle    = RegisterAgent(&agentInfo);
    const AgentInfo*          info      = GetAgentInfo(handle);
    ASSERT_NE(info, nullptr) << "AgentInfo should not be null";
    EXPECT_EQ(info->cu_num, 64u);
    EXPECT_EQ(info->se_num, 4u);
    EXPECT_EQ(info->xcc_num, 1u);
    EXPECT_EQ(info->shader_arrays_per_se, 2u);
}

TEST(Pm4FactoryTest, Gfx1151SqEventCountUsesPhysicalWgpSpan)
{
    AgentInfo agentInfo{};
    strcpy(agentInfo.name, "gfx1151");
    strcpy(agentInfo.gfxip, "gfx1151");
    agentInfo.cu_num               = 20;
    agentInfo.se_num               = 2;
    agentInfo.xcc_num              = 1;
    agentInfo.shader_arrays_per_se = 2;
    agentInfo.cu_bitmap.bits[0][0] = 0x03;  // 1 active WGP
    agentInfo.cu_bitmap.bits[0][1] = 0x0f;  // 2 active WGPs
    agentInfo.cu_bitmap.bits[1][0] = 0x3f;  // 3 active WGPs
    agentInfo.cu_bitmap.bits[1][1] = 0xff;  // 4 active WGPs

    Pm4Factory::Destroy();
    Pm4Factory* factory = Pm4Factory::Create(&agentInfo, GFX115X_GPU_ID, false);
    ASSERT_NE(factory, nullptr);

    // Upstream's harvested-WGP path preserves physical slots and zero-fills
    // harvested WGPs, so sizing uses the highest active WGP plus one per SA.
    EXPECT_EQ(factory->GetNumEvents(HSA_VEN_AMD_AQLPROFILE_BLOCK_NAME_SQ), 16u);
    EXPECT_EQ(factory->GetBytesNeeded(HSA_VEN_AMD_AQLPROFILE_BLOCK_NAME_SQ),
              16u * sizeof(uint64_t));
    EXPECT_EQ(factory->GetNumEvents(AQLPROFILE_BLOCK_NAME_SQG), 2u);
    EXPECT_EQ(factory->GetBytesNeeded(AQLPROFILE_BLOCK_NAME_SQG), 2u * sizeof(uint64_t));
    EXPECT_EQ(factory->GetNumWGPs(), 4);

    Pm4Factory::Destroy();
}

TEST(Pm4FactoryTest, Gfx1151MapsToGfx115xAndGfx11Behavior)
{
    EXPECT_EQ(Pm4Factory::GetGpuId("gfx1151"), GFX115X_GPU_ID);
    EXPECT_EQ(Pm4Factory::GetGpuId("gfx1150"), GFX115X_GPU_ID);
    EXPECT_EQ(Pm4Factory::GetGpuId("gfx1100"), GFX11_GPU_ID);
    EXPECT_TRUE((std::is_base_of<Gfx11Factory, Gfx115xFactory>::value));
}

TEST(Pm4FactoryTest, Gfx1151FactoryUsesGfx115xSqttStatus2Masks)
{
    aqlprofile_agent_info_v1_t agentInfo{};
    agentInfo.agent_gfxip          = "gfx1151";
    agentInfo.cu_num               = 32;
    agentInfo.se_num               = 4;
    agentInfo.xcc_num              = 1;
    agentInfo.shader_arrays_per_se = 2;
    agentInfo.domain               = 0;
    agentInfo.location_id          = 0x1234;
    Pm4Factory::Destroy();

    aqlprofile_agent_handle_t handle  = RegisterAgent(&agentInfo);
    Pm4Factory*               factory = Pm4Factory::Create(handle);
    ASSERT_NE(factory, nullptr);
    EXPECT_EQ(factory->GetGpuId(), GFX115X_GPU_ID);
    EXPECT_TRUE(factory->IsGFX11());
    ASSERT_NE(factory->GetSqttBuilder(), nullptr);
    EXPECT_EQ(factory->GetSqttBuilder()->GetBufferFullMask(),
              SQ_THREAD_TRACE_STATUS2__BUF0_FULL_MASK |
                  SQ_THREAD_TRACE_STATUS2__BUF1_FULL_MASK |
                  SQ_THREAD_TRACE_STATUS2__WRITE_BUF_FULL_MASK);
    EXPECT_EQ(factory->GetSqttBuilder()->GetLockDownFailMask(),
              SQ_THREAD_TRACE_STATUS2__PACKET_LOST_BUF_NO_LOCKDOWN_MASK);
    Pm4Factory::Destroy();
}

// Test: GetAgentInfo returns nullptr for invalid handle
TEST(Pm4FactoryTest, GetAgentInfoInvalidHandleReturnsNull)
{
    aqlprofile_agent_handle_t invalidHandle{};
    invalidHandle.handle  = 99999;  // unlikely to exist
    const AgentInfo* info = GetAgentInfo(invalidHandle);
    EXPECT_EQ(info, nullptr);
}
