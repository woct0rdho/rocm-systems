/*
* Copyright (c) Advanced Micro Devices, Inc., or its affiliates.
*
* SPDX-License-Identifier: MIT
*/

#include "suites/functional/pc_sampling.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "common/base_rocr_utils.h"
#include "common/common.h"
#include "common/helper_funcs.h"
#include "gtest/gtest.h"
#include "hsa/hsa.h"
#include "hsa/hsa_ext_amd.h"

namespace {

static const char kSubTestSeparator[] = "  **************************";

void PrintPcSamplingSubtestHeader(const char* header) {
  std::cout << "  *** PC Sampling Subtest: " << header << " ***" << std::endl;
}

std::string StatusString(hsa_status_t status) {
  const char* msg = nullptr;
  hsa_status_string(status, &msg);
  return msg ? msg : "unknown HSA status";
}

bool IsPcSamplingExtensionBitSet(const uint8_t extensions[128]) {
  const uint32_t bit = HSA_EXTENSION_AMD_PC_SAMPLING;
  return (extensions[bit / 8] & (1 << (bit % 8))) != 0;
}

bool AgentAdvertisesPcSampling(hsa_agent_t agent) {
  uint8_t extensions[128] = {0};
  hsa_status_t err = hsa_agent_get_info(agent, HSA_AGENT_INFO_EXTENSIONS, extensions);
  EXPECT_EQ(HSA_STATUS_SUCCESS, err) << StatusString(err);
  if (err != HSA_STATUS_SUCCESS) return false;

  return IsPcSamplingExtensionBitSet(extensions);
}

std::string AgentName(hsa_agent_t agent) {
  char name[64] = {0};
  hsa_status_t err = hsa_agent_get_info(agent, HSA_AGENT_INFO_NAME, name);
  EXPECT_EQ(HSA_STATUS_SUCCESS, err) << StatusString(err);
  if (err != HSA_STATUS_SUCCESS) return std::string();

  return std::string(name);
}

bool IsGfx1151(hsa_agent_t agent) {
  return AgentName(agent).find("gfx1151") != std::string::npos;
}

hsa_status_t CollectPcSamplingConfig(
    const hsa_ven_amd_pcs_configuration_t* configuration, void* data) {
  if (configuration == nullptr || data == nullptr) return HSA_STATUS_ERROR_INVALID_ARGUMENT;

  std::vector<hsa_ven_amd_pcs_configuration_t>* configs =
      reinterpret_cast<std::vector<hsa_ven_amd_pcs_configuration_t>*>(data);
  configs->push_back(*configuration);
  return HSA_STATUS_SUCCESS;
}

bool LoadPcSamplingTable(hsa_ven_amd_pc_sampling_1_00_pfn_t* table) {
  std::memset(table, 0, sizeof(*table));
  hsa_status_t err = hsa_system_get_major_extension_table(
      HSA_EXTENSION_AMD_PC_SAMPLING, 1, sizeof(*table), table);
  if (err != HSA_STATUS_SUCCESS) {
    std::cout << "PC sampling extension table unavailable: " << StatusString(err)
              << ". Skipping." << std::endl;
    std::cout << kSubTestSeparator << std::endl;
    return false;
  }

  EXPECT_NE(nullptr, table->hsa_ven_amd_pcs_iterate_configuration);
  EXPECT_NE(nullptr, table->hsa_ven_amd_pcs_create);
  EXPECT_NE(nullptr, table->hsa_ven_amd_pcs_create_from_id);
  EXPECT_NE(nullptr, table->hsa_ven_amd_pcs_destroy);
  EXPECT_NE(nullptr, table->hsa_ven_amd_pcs_start);
  EXPECT_NE(nullptr, table->hsa_ven_amd_pcs_stop);
  EXPECT_NE(nullptr, table->hsa_ven_amd_pcs_flush);

  return table->hsa_ven_amd_pcs_iterate_configuration != nullptr &&
         table->hsa_ven_amd_pcs_create != nullptr &&
         table->hsa_ven_amd_pcs_create_from_id != nullptr &&
         table->hsa_ven_amd_pcs_destroy != nullptr &&
         table->hsa_ven_amd_pcs_start != nullptr &&
         table->hsa_ven_amd_pcs_stop != nullptr &&
         table->hsa_ven_amd_pcs_flush != nullptr;
}

std::vector<hsa_agent_t> GpuAgents() {
  std::vector<hsa_agent_t> gpus;
  hsa_status_t err = hsa_iterate_agents(rocrtst::IterateGPUAgents, &gpus);
  EXPECT_EQ(HSA_STATUS_SUCCESS, err) << StatusString(err);
  return gpus;
}

std::vector<hsa_agent_t> Gfx1151Agents(const std::vector<hsa_agent_t>& agents) {
  std::vector<hsa_agent_t> gfx1151_agents;
  for (hsa_agent_t agent : agents) {
    if (IsGfx1151(agent)) gfx1151_agents.push_back(agent);
  }
  return gfx1151_agents;
}

void LogNoGfx1151Skip() {
  std::cout << "No gfx1151 GPU agent found. Skipping gfx1151 PC sampling checks."
            << std::endl;
  std::cout << kSubTestSeparator << std::endl;
}

bool IsValidMethod(hsa_ven_amd_pcs_method_kind_t method) {
  return method == HSA_VEN_AMD_PCS_METHOD_HOSTTRAP_V1 ||
         method == HSA_VEN_AMD_PCS_METHOD_STOCHASTIC_V1;
}

bool IsValidUnits(hsa_ven_amd_pcs_units_t units) {
  return units == HSA_VEN_AMD_PCS_INTERVAL_UNITS_MICRO_SECONDS ||
         units == HSA_VEN_AMD_PCS_INTERVAL_UNITS_CLOCK_CYCLES ||
         units == HSA_VEN_AMD_PCS_INTERVAL_UNITS_INSTRUCTIONS;
}

bool ChooseInterval(const hsa_ven_amd_pcs_configuration_t& config, size_t* interval) {
  if (config.max_interval == 0) return false;

  const size_t min_interval = std::max<size_t>(config.min_interval, 1);
  if (config.flags & HSA_VEN_AMD_PCS_CONFIGURATION_FLAGS_INTERVAL_POWER_OF_2) {
    size_t value = 1;
    while (value < min_interval) {
      if (value > std::numeric_limits<size_t>::max() / 2) return false;
      value *= 2;
    }
    if (value > config.max_interval) return false;
    *interval = value;
    return true;
  }

  if (min_interval > config.max_interval) return false;
  *interval = min_interval;
  return true;
}

void ValidatePcSamplingConfig(const hsa_ven_amd_pcs_configuration_t& config) {
  EXPECT_TRUE(IsValidMethod(config.method));
  EXPECT_TRUE(IsValidUnits(config.units));
  EXPECT_LE(config.min_interval, config.max_interval);
  EXPECT_GT(config.max_interval, 0u);

  size_t interval = 0;
  EXPECT_TRUE(ChooseInterval(config, &interval));
}

const hsa_ven_amd_pcs_configuration_t* FindConfig(
    const std::vector<hsa_ven_amd_pcs_configuration_t>& configs,
    hsa_ven_amd_pcs_method_kind_t method) {
  for (const hsa_ven_amd_pcs_configuration_t& config : configs) {
    size_t interval = 0;
    if (config.method == method && IsValidUnits(config.units) &&
        ChooseInterval(config, &interval)) {
      return &config;
    }
  }
  return nullptr;
}

bool IteratePcSamplingConfigs(const hsa_ven_amd_pc_sampling_1_00_pfn_t& table,
                              hsa_agent_t agent,
                              std::vector<hsa_ven_amd_pcs_configuration_t>* configs) {
  configs->clear();
  hsa_status_t err = table.hsa_ven_amd_pcs_iterate_configuration(
      agent, CollectPcSamplingConfig, configs);
  EXPECT_EQ(HSA_STATUS_SUCCESS, err) << StatusString(err);
  return err == HSA_STATUS_SUCCESS;
}

size_t SampleSize(hsa_ven_amd_pcs_method_kind_t method) {
  return method == HSA_VEN_AMD_PCS_METHOD_HOSTTRAP_V1
             ? sizeof(perf_sample_hosttrap_v1_t)
             : sizeof(perf_sample_snapshot_v1_t);
}

const char* MethodName(hsa_ven_amd_pcs_method_kind_t method) {
  return method == HSA_VEN_AMD_PCS_METHOD_HOSTTRAP_V1 ? "hosttrap" : "stochastic";
}

struct CallbackState {
  explicit CallbackState(size_t sample_size) : sample_size(sample_size) {}

  const size_t sample_size;
  std::atomic<size_t> ready_callbacks{0};
  std::atomic<size_t> bytes_copied{0};
  std::atomic<size_t> lost_samples{0};
  std::atomic<bool> bad_sample_shape{false};
  std::atomic<bool> copy_failed{false};
};

void PcSamplingDataReadyCallback(
    void* client_callback_data, size_t data_size, size_t lost_sample_count,
    hsa_ven_amd_pcs_data_copy_callback_t data_copy_callback, void* hsa_callback_data) {
  CallbackState* state = reinterpret_cast<CallbackState*>(client_callback_data);
  if (state == nullptr) return;

  state->ready_callbacks.fetch_add(1);
  state->lost_samples.fetch_add(lost_sample_count);

  if (data_size == 0) return;
  if (data_size % state->sample_size != 0) state->bad_sample_shape = true;

  std::vector<uint8_t> data(data_size);
  hsa_status_t err = data_copy_callback(hsa_callback_data, data.size(), data.data());
  if (err != HSA_STATUS_SUCCESS) {
    state->copy_failed = true;
    return;
  }

  state->bytes_copied.fetch_add(data_size);
}

class PcSamplingSessionGuard {
 public:
  PcSamplingSessionGuard(const hsa_ven_amd_pc_sampling_1_00_pfn_t* table,
                         hsa_ven_amd_pcs_t session)
      : table_(table), session_(session) {}

  ~PcSamplingSessionGuard() {
    if (table_ != nullptr && session_.handle != 0) {
      table_->hsa_ven_amd_pcs_destroy(session_);
    }
  }

  hsa_ven_amd_pcs_t get() const { return session_; }

  void Release() { session_.handle = 0; }

 private:
  const hsa_ven_amd_pc_sampling_1_00_pfn_t* table_;
  hsa_ven_amd_pcs_t session_;
};

void ExerciseLifecycle(const hsa_ven_amd_pc_sampling_1_00_pfn_t& table, hsa_agent_t agent,
                       const hsa_ven_amd_pcs_configuration_t& config) {
  size_t interval = 0;
  ASSERT_TRUE(ChooseInterval(config, &interval));

  CallbackState callback_state(SampleSize(config.method));
  hsa_ven_amd_pcs_t session = {0};

  const size_t buffer_size = 4 * callback_state.sample_size;
  hsa_status_t err = table.hsa_ven_amd_pcs_create(
      agent, config.method, config.units, interval, 0, buffer_size,
      PcSamplingDataReadyCallback, &callback_state, &session);
  ASSERT_EQ(HSA_STATUS_SUCCESS, err)
      << StatusString(err) << " while creating " << MethodName(config.method)
      << " PC sampling session";
  ASSERT_NE(0u, session.handle);

  PcSamplingSessionGuard guard(&table, session);

  ASSERT_EQ(HSA_STATUS_SUCCESS, table.hsa_ven_amd_pcs_start(session))
      << "Failed to start " << MethodName(config.method) << " PC sampling session";
  ASSERT_EQ(HSA_STATUS_SUCCESS, table.hsa_ven_amd_pcs_start(session))
      << "Starting an active PC sampling session should be a no-op";
  ASSERT_EQ(HSA_STATUS_SUCCESS, table.hsa_ven_amd_pcs_stop(session))
      << "Failed to stop " << MethodName(config.method) << " PC sampling session";
  ASSERT_EQ(HSA_STATUS_SUCCESS, table.hsa_ven_amd_pcs_stop(session))
      << "Stopping an inactive PC sampling session should be a no-op";
  ASSERT_EQ(HSA_STATUS_SUCCESS, table.hsa_ven_amd_pcs_flush(session))
      << "Failed to flush stopped PC sampling session";
  ASSERT_EQ(HSA_STATUS_SUCCESS, table.hsa_ven_amd_pcs_start(session))
      << "Failed to restart " << MethodName(config.method) << " PC sampling session";
  ASSERT_EQ(HSA_STATUS_SUCCESS, table.hsa_ven_amd_pcs_stop(session))
      << "Failed to stop restarted PC sampling session";
  ASSERT_EQ(HSA_STATUS_SUCCESS, table.hsa_ven_amd_pcs_flush(session))
      << "Failed to flush restarted PC sampling session";

  EXPECT_FALSE(callback_state.bad_sample_shape.load())
      << "PC sampling callback data size was not a whole number of samples";
  EXPECT_FALSE(callback_state.copy_failed.load())
      << "PC sampling data copy callback failed";

  ASSERT_EQ(HSA_STATUS_SUCCESS, table.hsa_ven_amd_pcs_destroy(session))
      << "Failed to destroy " << MethodName(config.method) << " PC sampling session";
  guard.Release();
}

}  // namespace

PcSamplingTest::PcSamplingTest(void) : TestBase() {
  set_num_iteration(1);
  set_title("RocR PC Sampling Tests");
  set_description("Validates PC sampling extension table, gfx1151 configurations, "
                  "and basic hosttrap/stochastic session lifecycle.");
}

PcSamplingTest::~PcSamplingTest(void) {}

void PcSamplingTest::SetUp(void) {
  TestBase::SetUp();
  if (test_skipped_) return;
}

void PcSamplingTest::Run(void) {
  if (!rocrtst::CheckProfile(this)) return;
  TestBase::Run();
}

void PcSamplingTest::DisplayTestInfo(void) {
  TestBase::DisplayTestInfo();
}

void PcSamplingTest::DisplayResults(void) const {
  if (!rocrtst::CheckProfile(this)) return;
  TestBase::DisplayResults();
}

void PcSamplingTest::Close() {
  TestBase::Close();
}

void PcSamplingTest::ExtensionAndConfigTest(void) {
  PrintPcSamplingSubtestHeader("ExtensionAndConfigTest");

  std::vector<hsa_agent_t> gpus = GpuAgents();
  std::vector<hsa_agent_t> gfx1151_agents = Gfx1151Agents(gpus);
  if (gfx1151_agents.empty()) {
    LogNoGfx1151Skip();
    return;
  }

  hsa_ven_amd_pc_sampling_1_00_pfn_t table;
  ASSERT_TRUE(LoadPcSamplingTable(&table));

  for (hsa_agent_t agent : gfx1151_agents) {
    ASSERT_TRUE(AgentAdvertisesPcSampling(agent))
        << "gfx1151 agent does not advertise HSA_EXTENSION_AMD_PC_SAMPLING";

    std::vector<hsa_ven_amd_pcs_configuration_t> configs;
    ASSERT_TRUE(IteratePcSamplingConfigs(table, agent, &configs));
    ASSERT_FALSE(configs.empty()) << "gfx1151 PC sampling configuration list is empty";

    for (const hsa_ven_amd_pcs_configuration_t& config : configs) {
      ValidatePcSamplingConfig(config);
    }

    EXPECT_NE(nullptr, FindConfig(configs, HSA_VEN_AMD_PCS_METHOD_HOSTTRAP_V1))
        << "gfx1151 missing HOSTTRAP_V1 PC sampling configuration";
    EXPECT_NE(nullptr, FindConfig(configs, HSA_VEN_AMD_PCS_METHOD_STOCHASTIC_V1))
        << "gfx1151 missing STOCHASTIC_V1 PC sampling configuration";
  }
}

void PcSamplingTest::LifecycleTest(void) {
  PrintPcSamplingSubtestHeader("LifecycleTest");

  std::vector<hsa_agent_t> gfx1151_agents = Gfx1151Agents(GpuAgents());
  if (gfx1151_agents.empty()) {
    LogNoGfx1151Skip();
    return;
  }

  hsa_ven_amd_pc_sampling_1_00_pfn_t table;
  ASSERT_TRUE(LoadPcSamplingTable(&table));

  for (hsa_agent_t agent : gfx1151_agents) {
    std::vector<hsa_ven_amd_pcs_configuration_t> configs;
    ASSERT_TRUE(IteratePcSamplingConfigs(table, agent, &configs));

    const hsa_ven_amd_pcs_configuration_t* hosttrap_config =
        FindConfig(configs, HSA_VEN_AMD_PCS_METHOD_HOSTTRAP_V1);
    ASSERT_NE(nullptr, hosttrap_config)
        << "gfx1151 missing HOSTTRAP_V1 PC sampling configuration";
    ExerciseLifecycle(table, agent, *hosttrap_config);

    const hsa_ven_amd_pcs_configuration_t* stochastic_config =
        FindConfig(configs, HSA_VEN_AMD_PCS_METHOD_STOCHASTIC_V1);
    ASSERT_NE(nullptr, stochastic_config)
        << "gfx1151 missing STOCHASTIC_V1 PC sampling configuration";
    ExerciseLifecycle(table, agent, *stochastic_config);
  }
}
