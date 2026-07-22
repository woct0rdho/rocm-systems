////////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// MIT License
//
////////////////////////////////////////////////////////////////////////////////

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "impl/wddm/profiling.h"

namespace {

using wsl::thunk::profiling::AqlProfilePacket;
using wsl::thunk::profiling::Capability;
using wsl::thunk::profiling::ClassifyVendorPacket;
using wsl::thunk::profiling::CommandChecksum;
using wsl::thunk::profiling::DetectCapability;
using wsl::thunk::profiling::ResolvedMemory;
using wsl::thunk::profiling::SelectDispatchTimestampTargets;
using wsl::thunk::profiling::Stage;
using wsl::thunk::profiling::SubmissionState;
using wsl::thunk::profiling::ValidatePacket;
using wsl::thunk::profiling::ValidateRuntimePacket;
using wsl::thunk::profiling::ValidationStatus;
using wsl::thunk::profiling::VendorPacketKind;

constexpr uint32_t Type3(uint32_t opcode, uint32_t dwords) {
  return (3u << 30) | (opcode << 8) | ((dwords - 2) << 16);
}

struct Range {
  uint64_t address;
  uint64_t size;
  const void* cpu;
  uintptr_t owner;
};

struct Resolver {
  std::vector<Range> ranges;

  bool operator()(uint64_t address, uint64_t size, ResolvedMemory* resolved) {
    if (size > UINT64_MAX - address) return false;
    for (const auto& range : ranges) {
      if (address >= range.address && address + size <= range.address + range.size) {
        resolved->cpu_address = static_cast<const uint8_t*>(range.cpu) + (address - range.address);
        resolved->owner = range.owner;
        return true;
      }
    }
    return false;
  }
};

AqlProfilePacket MakePacket(const std::vector<uint32_t>& commands, Stage stage,
                            uint64_t profile_key = 0x123456789abcdef0ull) {
  const uint64_t address = reinterpret_cast<uint64_t>(commands.data());
  AqlProfilePacket packet{};
  packet.format = wsl::thunk::profiling::kAqlProfileIbFormat;
  packet.ib[0] = Type3(0x3f, 4);
  packet.ib[1] = static_cast<uint32_t>(address) & ~3u;
  packet.ib[2] = static_cast<uint32_t>(address >> 32);
  packet.ib[3] = (1u << 23) | (1u << 28) | static_cast<uint32_t>(commands.size());
  packet.dwords_remaining = 10;
  packet.reserved[0] = wsl::thunk::profiling::kManifestMagic;
  packet.reserved[1] = wsl::thunk::profiling::kManifestVersion;
  packet.reserved[2] = static_cast<uint32_t>(stage);
  packet.reserved[3] = static_cast<uint32_t>(commands.size());
  packet.reserved[4] = CommandChecksum(commands.data(), static_cast<uint32_t>(commands.size()));
  packet.reserved[5] = static_cast<uint32_t>(profile_key);
  packet.reserved[6] = static_cast<uint32_t>(profile_key >> 32);
  packet.reserved[7] = (1u << 8) | wsl::thunk::profiling::kManifestRequiredFlags;
  return packet;
}

AqlProfilePacket MakeRuntimePacket(const std::vector<uint32_t>& commands) {
  const uint64_t address = reinterpret_cast<uint64_t>(commands.data());
  AqlProfilePacket packet{};
  packet.format = wsl::thunk::profiling::kAqlProfileIbFormat;
  packet.ib[0] = Type3(0x3f, 4);
  packet.ib[1] = static_cast<uint32_t>(address) & ~3u;
  packet.ib[2] = static_cast<uint32_t>(address >> 32);
  packet.ib[3] = (1u << 23) | static_cast<uint32_t>(commands.size());
  packet.dwords_remaining = 10;
  packet.reserved[0] = wsl::thunk::profiling::kRuntimeManifestMagic;
  packet.reserved[1] = wsl::thunk::profiling::kRuntimeManifestVersion;
  packet.reserved[2] = static_cast<uint32_t>(commands.size());
  packet.reserved[3] = CommandChecksum(commands.data(), static_cast<uint32_t>(commands.size()));
  return packet;
}

bool Expect(bool condition, const char* label, uint32_t* checks, uint32_t* failures) {
  ++*checks;
  if (!condition) {
    ++*failures;
    std::fprintf(stderr, "profiling adapter check failed: %s\n", label);
  }
  return condition;
}

}  // namespace

int main() {
  uint32_t checks = 0;
  uint32_t failures = 0;

  const Capability capability = DetectCapability(11, 5, 1, 0x500, true);
  Expect(capability.supported && capability.version == 1 && capability.max_pm4_dwords == 256,
         "gfx1151 capability", &checks, &failures);
  Expect(!DetectCapability(11, 5, 0, 0x500, true).supported, "unsupported stepping", &checks,
         &failures);
  Expect(!DetectCapability(11, 5, 1, 0x4ff, true).supported, "undersized frame", &checks,
         &failures);
  Expect(!DetectCapability(11, 5, 1, 0x500, false).supported, "missing submission capability",
         &checks, &failures);
  Expect(DetectCapability(11, 5, 1, 0x900, true).max_pm4_dwords == 512,
         "frame-derived command capacity", &checks, &failures);
  Expect(DetectCapability(11, 5, 1, wsl::thunk::profiling::kQualifiedFrameBytes, true)
                 .max_pm4_dwords == 1984,
         "qualified command capacity", &checks, &failures);
  Expect(DetectCapability(11, 5, 1, 0x4000, true).max_pm4_dwords == 1984,
         "bounded command capacity", &checks, &failures);

  uint64_t dispatch_start = 0;
  uint64_t dispatch_end = 0;
  const auto enabled_timestamps =
      SelectDispatchTimestampTargets(true, &dispatch_start, &dispatch_end);
  Expect(enabled_timestamps.start == &dispatch_start && enabled_timestamps.end == &dispatch_end,
         "profiling timestamp signal targets", &checks, &failures);
  const auto disabled_timestamps =
      SelectDispatchTimestampTargets(false, &dispatch_start, &dispatch_end);
  Expect(disabled_timestamps.start == nullptr && disabled_timestamps.end == nullptr,
         "disabled profiling timestamps", &checks, &failures);
  const auto missing_start = SelectDispatchTimestampTargets(true, nullptr, &dispatch_end);
  Expect(missing_start.start == nullptr && missing_start.end == nullptr,
         "missing start timestamp", &checks, &failures);
  const auto missing_end = SelectDispatchTimestampTargets(true, &dispatch_start, nullptr);
  Expect(missing_end.start == nullptr && missing_end.end == nullptr,
         "missing end timestamp", &checks, &failures);

  alignas(8) std::array<uint64_t, 4> output{};
  const uint64_t output_address = reinterpret_cast<uint64_t>(output.data());
  std::vector<uint32_t> start = {Type3(0x79, 3), 0, 1};
  std::vector<uint32_t> read = {Type3(0x40, 6), 2u << 8, 0x2000, 0,
                                static_cast<uint32_t>(output_address),
                                static_cast<uint32_t>(output_address >> 32)};
  std::vector<uint32_t> stop = {Type3(0x79, 3), 0, 0};

  Resolver resolver{{
      {reinterpret_cast<uint64_t>(start.data()), start.size() * sizeof(uint32_t), start.data(), 1},
      {reinterpret_cast<uint64_t>(read.data()), read.size() * sizeof(uint32_t), read.data(), 2},
      {reinterpret_cast<uint64_t>(stop.data()), stop.size() * sizeof(uint32_t), stop.data(), 3},
      {output_address, sizeof(output), output.data(), 4},
  }};
  auto signal = [](uint64_t handle) { return handle == 0x1000; };

  auto start_packet = MakePacket(start, Stage::kStart);
  auto read_packet = MakePacket(read, Stage::kRead);
  auto stop_packet = MakePacket(stop, Stage::kStop);
  Expect(ValidatePacket(capability, start_packet, resolver, signal).status ==
             ValidationStatus::kSuccess,
         "valid start", &checks, &failures);
  Expect(ValidatePacket(capability, read_packet, resolver, signal).status ==
             ValidationStatus::kSuccess,
         "valid read", &checks, &failures);
  Expect(ValidatePacket(capability, stop_packet, resolver, signal).status ==
             ValidationStatus::kSuccess,
         "valid stop", &checks, &failures);

  auto runtime_packet = MakeRuntimePacket(start);
  Expect(ClassifyVendorPacket(start_packet) == VendorPacketKind::kProfile,
         "profile routing", &checks, &failures);
  Expect(ClassifyVendorPacket(runtime_packet) == VendorPacketKind::kRuntime,
         "runtime PM4 routing", &checks, &failures);
  auto ordinary_vendor = AqlProfilePacket{};
  ordinary_vendor.format = wsl::thunk::profiling::kBarrierValueFormat;
  Expect(ClassifyVendorPacket(ordinary_vendor) == VendorPacketKind::kBarrierValue,
         "barrier-value routing", &checks, &failures);
  ordinary_vendor.format = wsl::thunk::profiling::kExtendedDispatchFormat;
  Expect(ClassifyVendorPacket(ordinary_vendor) == VendorPacketKind::kExtendedDispatch,
         "extended-dispatch routing", &checks, &failures);
  ordinary_vendor.format = 0x7f;
  Expect(ClassifyVendorPacket(ordinary_vendor) == VendorPacketKind::kOther,
         "other vendor routing", &checks, &failures);
  Expect(ValidateRuntimePacket(capability, runtime_packet, resolver, signal).status ==
             ValidationStatus::kSuccess,
         "valid runtime PM4", &checks, &failures);
  auto malformed_runtime = runtime_packet;
  malformed_runtime.reserved[3] ^= 1;
  Expect(ValidateRuntimePacket(capability, malformed_runtime, resolver, signal).status ==
             ValidationStatus::kInvalidCommandBuffer,
         "runtime checksum rejection", &checks, &failures);
  malformed_runtime = runtime_packet;
  malformed_runtime.reserved[7] = 1;
  Expect(ValidateRuntimePacket(capability, malformed_runtime, resolver, signal).status ==
             ValidationStatus::kInvalidManifest,
         "runtime reserved rejection", &checks, &failures);

  SubmissionState state;
  Expect(state.Advance(Stage::kRead, 0x123456789abcdef0ull) ==
             ValidationStatus::kInvalidSequence,
         "read before start", &checks, &failures);
  Expect(state.Advance(Stage::kStart, 0x123456789abcdef0ull) ==
             ValidationStatus::kSuccess,
         "start sequence", &checks, &failures);
  Expect(state.Advance(Stage::kStart, 0x123456789abcdef0ull) ==
             ValidationStatus::kInvalidSequence,
         "double start", &checks, &failures);
  Expect(state.Advance(Stage::kRead, 0xfedcba9876543210ull) ==
             ValidationStatus::kInvalidSequence,
         "mismatched read", &checks, &failures);
  Expect(state.Advance(Stage::kRead, 0x123456789abcdef0ull) ==
             ValidationStatus::kSuccess,
         "ordered read", &checks, &failures);
  Expect(state.Advance(Stage::kStop, 0x123456789abcdef0ull) ==
             ValidationStatus::kSuccess,
         "ordered stop", &checks, &failures);
  Expect(state.Advance(Stage::kStart, 0xfedcba9876543210ull) ==
             ValidationStatus::kSuccess,
         "repeated start", &checks, &failures);
  Expect(state.Advance(Stage::kRead, 0xfedcba9876543210ull) ==
             ValidationStatus::kSuccess,
         "repeated read", &checks, &failures);
  Expect(state.Advance(Stage::kStop, 0xfedcba9876543210ull) ==
             ValidationStatus::kSuccess,
         "repeated stop", &checks, &failures);

  auto malformed = start_packet;
  malformed.header = 1;
  Expect(ValidatePacket(capability, malformed, resolver, signal).status ==
             ValidationStatus::kInvalidFormat,
         "packet type rejection", &checks, &failures);
  malformed = start_packet;
  malformed.format = 2;
  Expect(ValidatePacket(capability, malformed, resolver, signal).status ==
             ValidationStatus::kInvalidFormat,
         "format rejection", &checks, &failures);
  malformed = start_packet;
  malformed.dwords_remaining = 9;
  Expect(ValidatePacket(capability, malformed, resolver, signal).status ==
             ValidationStatus::kInvalidFormat,
         "remaining size rejection", &checks, &failures);
  malformed = start_packet;
  malformed.reserved[0] = 0;
  Expect(ValidatePacket(capability, malformed, resolver, signal).status ==
             ValidationStatus::kInvalidManifest,
         "manifest rejection", &checks, &failures);
  malformed = start_packet;
  malformed.ib[0] = Type3(0x40, 4);
  Expect(ValidatePacket(capability, malformed, resolver, signal).status ==
             ValidationStatus::kInvalidIndirectBuffer,
         "IB opcode rejection", &checks, &failures);
  malformed = start_packet;
  malformed.ib[3] |= 1u << 20;
  Expect(ValidatePacket(capability, malformed, resolver, signal).status ==
             ValidationStatus::kInvalidIndirectBuffer,
         "IB control rejection", &checks, &failures);
  malformed = start_packet;
  malformed.ib[1] += 2;
  Expect(ValidatePacket(capability, malformed, resolver, signal).status ==
             ValidationStatus::kInvalidIndirectBuffer,
         "IB alignment rejection", &checks, &failures);
  malformed = start_packet;
  malformed.reserved[7] |= 4;
  Expect(ValidatePacket(capability, malformed, resolver, signal).status ==
             ValidationStatus::kInvalidManifest,
         "validation flags rejection", &checks, &failures);
  malformed = start_packet;
  malformed.reserved[4] ^= 1;
  Expect(ValidatePacket(capability, malformed, resolver, signal).status ==
             ValidationStatus::kInvalidCommandBuffer,
         "checksum rejection", &checks, &failures);
  malformed = start_packet;
  malformed.completion_signal = 0x2000;
  Expect(ValidatePacket(capability, malformed, resolver, signal).status ==
             ValidationStatus::kInvalidCompletionSignal,
         "completion ownership rejection", &checks, &failures);
  malformed.completion_signal = 0x1000;
  Expect(ValidatePacket(capability, malformed, resolver, signal).status ==
             ValidationStatus::kSuccess,
         "owned completion signal", &checks, &failures);

  Resolver missing_output = resolver;
  missing_output.ranges.pop_back();
  Expect(ValidatePacket(capability, read_packet, missing_output, signal).status ==
             ValidationStatus::kInvalidMemory,
         "result allocation rejection", &checks, &failures);
  Resolver missing_commands{};
  Expect(ValidatePacket(capability, start_packet, missing_commands, signal).status ==
             ValidationStatus::kInvalidCommandBuffer,
         "command allocation rejection", &checks, &failures);

  std::vector<uint32_t> invalid_opcode = {Type3(0x20, 3), 0, 0};
  Resolver opcode_resolver{{{reinterpret_cast<uint64_t>(invalid_opcode.data()),
                             invalid_opcode.size() * sizeof(uint32_t),
                             invalid_opcode.data(), 5}}};
  auto opcode_packet = MakePacket(invalid_opcode, Stage::kStart);
  Expect(ValidatePacket(capability, opcode_packet, opcode_resolver, signal).status ==
             ValidationStatus::kInvalidCommand,
         "opcode rejection", &checks, &failures);

  std::vector<uint32_t> invalid_register = {Type3(0x79, 3), 0x4000, 0};
  Resolver register_resolver{{{reinterpret_cast<uint64_t>(invalid_register.data()),
                               invalid_register.size() * sizeof(uint32_t),
                               invalid_register.data(), 6}}};
  auto register_packet = MakePacket(invalid_register, Stage::kStart);
  Expect(ValidatePacket(capability, register_packet, register_resolver, signal).status ==
             ValidationStatus::kInvalidCommand,
         "register rejection", &checks, &failures);

  auto unaligned_read = read;
  unaligned_read[4] += 2;
  Resolver unaligned_resolver = resolver;
  unaligned_resolver.ranges.push_back({reinterpret_cast<uint64_t>(unaligned_read.data()),
                                       unaligned_read.size() * sizeof(uint32_t),
                                       unaligned_read.data(), 7});
  auto unaligned_packet = MakePacket(unaligned_read, Stage::kRead);
  Expect(ValidatePacket(capability, unaligned_packet, unaligned_resolver, signal).status ==
             ValidationStatus::kInvalidMemory,
         "result alignment rejection", &checks, &failures);

  std::vector<uint32_t> nested = {Type3(0x3f, 4), 0, 0, 0};
  Resolver nested_resolver{{{reinterpret_cast<uint64_t>(nested.data()),
                             nested.size() * sizeof(uint32_t), nested.data(), 8}}};
  auto nested_packet = MakePacket(nested, Stage::kStart);
  Expect(ValidatePacket(capability, nested_packet, nested_resolver, signal).status ==
             ValidationStatus::kInvalidCommand,
         "nested IB rejection", &checks, &failures);

  std::vector<uint32_t> truncated = {Type3(0x79, 3), 0};
  Resolver truncated_resolver{{{reinterpret_cast<uint64_t>(truncated.data()),
                                truncated.size() * sizeof(uint32_t), truncated.data(), 9}}};
  auto truncated_packet = MakePacket(truncated, Stage::kStart);
  Expect(ValidatePacket(capability, truncated_packet, truncated_resolver, signal).status ==
             ValidationStatus::kInvalidCommand,
         "truncated packet rejection", &checks, &failures);

  Expect(ValidatePacket(Capability{}, start_packet, resolver, signal).status ==
             ValidationStatus::kUnsupportedCapability,
         "unsupported capability status", &checks, &failures);

  std::printf("profiling_adapter checks=%u failures=%u capability_version=%u max_dwords=%u\n",
              checks, failures, capability.version, capability.max_pm4_dwords);
  return failures == 0 ? 0 : 1;
}
