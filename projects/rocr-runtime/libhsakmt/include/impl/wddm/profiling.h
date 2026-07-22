////////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// MIT LICENSE:
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstddef>
#include <cstdint>

namespace wsl {
namespace thunk {
namespace profiling {

constexpr uint32_t kMinimumFrameBytes = 0x500;
constexpr uint32_t kQualifiedFrameBytes = 0x2000;
constexpr uint32_t kFrameTrailerReserveBytes = 0x100;
constexpr uint32_t kAqlProfileIbFormat = 1;
constexpr uint32_t kManifestMagic = 0x504d4357u;  // "WCMP"
constexpr uint32_t kManifestVersion = 1;
constexpr uint32_t kRuntimeManifestMagic = 0x4d545257u;  // "WRTM"
constexpr uint32_t kRuntimeManifestVersion = 1;
constexpr uint8_t kBarrierValueFormat = 2;
constexpr uint8_t kExtendedDispatchFormat = 3;
constexpr uint32_t kManifestRequiredFlags = 0x3;
constexpr uint32_t kMaximumPm4Dwords =
    (kQualifiedFrameBytes - kFrameTrailerReserveBytes) / sizeof(uint32_t);

struct Capability {
  bool supported = false;
  uint32_t version = 0;
  uint32_t max_pm4_dwords = 0;
};

struct DispatchTimestampTargets {
  uint64_t* start = nullptr;
  uint64_t* end = nullptr;
};

constexpr DispatchTimestampTargets SelectDispatchTimestampTargets(
    bool profiling_enabled, uint64_t* start, uint64_t* end) {
  return profiling_enabled && start != nullptr && end != nullptr
             ? DispatchTimestampTargets{start, end}
             : DispatchTimestampTargets{};
}

constexpr Capability DetectCapability(uint32_t major, uint32_t minor, uint32_t stepping,
                                      uint32_t frame_bytes, bool compute_submission_supported) {
  // Native WDDM profiling is qualified only for gfx1151. Additional targets must
  // define and validate their packet policy before joining this capability set.
  const bool gfx1151 = major == 11 && minor == 5 && stepping == 1;
  const bool frame_is_sufficient = frame_bytes >= kMinimumFrameBytes;
  const bool supported = gfx1151 && frame_is_sufficient && compute_submission_supported;
  const uint32_t available_dwords =
      supported ? (frame_bytes - kFrameTrailerReserveBytes) / sizeof(uint32_t) : 0u;
  return {supported,
          supported ? 1u : 0u,
          available_dwords < kMaximumPm4Dwords ? available_dwords : kMaximumPm4Dwords};
}

struct AqlProfilePacket {
  uint16_t header;
  uint16_t format;
  uint32_t ib[4];
  uint32_t dwords_remaining;
  uint32_t reserved[8];
  uint64_t completion_signal;
};
static_assert(sizeof(AqlProfilePacket) == 64, "AQL packets are exactly 64 bytes");

constexpr bool IsVendorPacketHeader(uint16_t header) {
  const uint32_t type = header & 0xff;
  const uint32_t acquire_scope = (header >> 9) & 0x3;
  const uint32_t release_scope = (header >> 11) & 0x3;
  return type == 0 && acquire_scope <= 2 && release_scope <= 2 && (header & 0xe000) == 0;
}

enum class VendorPacketKind {
  kProfile,
  kRuntime,
  kBarrierValue,
  kExtendedDispatch,
  kOther,
};

constexpr VendorPacketKind ClassifyVendorPacket(const AqlProfilePacket& packet) {
  if (!IsVendorPacketHeader(packet.header)) return VendorPacketKind::kOther;
  const uint8_t amd_format = static_cast<uint8_t>(packet.format);
  if (amd_format == kBarrierValueFormat) return VendorPacketKind::kBarrierValue;
  if (amd_format == kExtendedDispatchFormat) return VendorPacketKind::kExtendedDispatch;
  if (packet.format != kAqlProfileIbFormat) return VendorPacketKind::kOther;
  return packet.reserved[0] == kRuntimeManifestMagic ? VendorPacketKind::kRuntime
                                                     : VendorPacketKind::kProfile;
}

enum class Stage : uint32_t {
  kNone = 0,
  kStart = 1,
  kRead = 2,
  kStop = 3,
};

enum class ValidationStatus {
  kSuccess,
  kUnsupportedCapability,
  kInvalidFormat,
  kInvalidManifest,
  kInvalidIndirectBuffer,
  kInvalidCommandBuffer,
  kInvalidCommand,
  kInvalidMemory,
  kInvalidCompletionSignal,
  kInvalidSequence,
};

struct ResolvedMemory {
  const void* cpu_address = nullptr;
  uintptr_t owner = 0;
};

struct ValidationResult {
  ValidationStatus status = ValidationStatus::kInvalidFormat;
  Stage stage = Stage::kNone;
  uint64_t profile_key = 0;
  uint32_t event_count = 0;
  uint32_t command_dwords = 0;
};

constexpr uint32_t CommandChecksum(const uint32_t* commands, uint32_t dword_count) {
  uint32_t value = 2166136261u;
  for (uint32_t i = 0; i < dword_count; ++i) {
    value ^= commands[i];
    value *= 16777619u;
  }
  return value;
}

namespace detail {

constexpr uint32_t kType3 = 3;
constexpr uint32_t kType3Nop = 0x10;
constexpr uint32_t kType3AtomicMem = 0x1e;
constexpr uint32_t kType3WriteData = 0x37;
constexpr uint32_t kType3WaitRegMem = 0x3c;
constexpr uint32_t kType3IndirectBuffer = 0x3f;
constexpr uint32_t kType3CopyData = 0x40;
constexpr uint32_t kType3EventWrite = 0x46;
constexpr uint32_t kType3AcquireMem = 0x58;
constexpr uint32_t kType3SetShReg = 0x76;
constexpr uint32_t kType3SetUconfigReg = 0x79;

constexpr uint32_t PacketType(uint32_t header) { return header >> 30; }
constexpr uint32_t PacketOpcode(uint32_t header) { return (header >> 8) & 0xff; }
constexpr uint32_t PacketCount(uint32_t header) { return (header >> 16) & 0x3fff; }

constexpr bool IsRegisterOffset(uint32_t value) {
  // AQL Profile uses the gfx11 config, persistent, and uconfig register spaces.
  // Requiring an 18-bit register offset prevents memory destinations and malformed
  // packet control bits from being interpreted as register programming.
  return value >= 0x2000 && value <= 0xffff;
}

constexpr bool AddWithoutOverflow(uint64_t base, uint64_t size) {
  return size <= UINT64_MAX - base;
}

template <typename Resolver>
bool ResolveMemory(Resolver& resolve, uint64_t address, uint64_t size,
                   const void** cpu_address = nullptr) {
  if (address == 0 || size == 0 || !AddWithoutOverflow(address, size)) return false;
  ResolvedMemory resolved{};
  if (!resolve(address, size, &resolved) || resolved.cpu_address == nullptr) return false;
  if (cpu_address) *cpu_address = resolved.cpu_address;
  return true;
}

struct StreamProperties {
  uint32_t register_writes = 0;
  uint32_t result_writes = 0;
};

template <typename Resolver>
ValidationStatus ValidateStream(const uint32_t* commands, uint32_t dword_count, Stage stage,
                                Resolver& resolve, StreamProperties* properties) {
  uint32_t offset = 0;
  while (offset < dword_count) {
    const uint32_t header = commands[offset];
    if (PacketType(header) != kType3) return ValidationStatus::kInvalidCommand;

    const uint32_t opcode = PacketOpcode(header);
    const uint32_t encoded_count = PacketCount(header);
    const uint32_t canonical_header =
        (kType3 << 30) | (opcode << 8) | (encoded_count << 16);
    if (header != canonical_header) return ValidationStatus::kInvalidCommand;
    const uint32_t packet_dwords =
        (opcode == kType3Nop && encoded_count == 0x3fff) ? 1u : encoded_count + 2u;
    if (packet_dwords == 0 || packet_dwords > dword_count - offset)
      return ValidationStatus::kInvalidCommand;

    const uint32_t* packet = commands + offset;
    switch (opcode) {
      case kType3Nop:
        break;
      case kType3EventWrite:
        if (packet_dwords != 2 || packet[1] != 0x407)
          return ValidationStatus::kInvalidCommand;
        break;
      case kType3AcquireMem: {
        if (packet_dwords != 8) return ValidationStatus::kInvalidCommand;
        const uint64_t chunks = static_cast<uint64_t>(packet[2]) |
                                (static_cast<uint64_t>(packet[3]) << 32);
        const uint64_t address = (static_cast<uint64_t>(packet[4]) << 8) |
                                 (static_cast<uint64_t>(packet[5] & 0xffffff) << 40);
        // ACQUIRE_MEM operates at 256-byte cache-line granularity, so a valid
        // allocation smaller than one chunk is expected to be covered by a
        // hardware range which extends past its exact end. Validate the owned
        // base and arithmetic; unlike COPY_DATA, this packet does not access or
        // modify every byte in the encoded range.
        if (chunks != 0 &&
            (chunks > (UINT64_MAX >> 8) || !AddWithoutOverflow(address, chunks << 8) ||
             !ResolveMemory(resolve, address, 1)))
          return ValidationStatus::kInvalidMemory;
        break;
      }
      case kType3WaitRegMem: {
        if (packet_dwords != 7) return ValidationStatus::kInvalidCommand;
        const bool memory_space = ((packet[1] >> 4) & 0x3) == 1;
        if (memory_space) {
          const uint64_t address = static_cast<uint64_t>(packet[2] & 0xfffffffc) |
                                   (static_cast<uint64_t>(packet[3]) << 32);
          if ((address & 0x3) != 0 || !ResolveMemory(resolve, address, sizeof(uint32_t)))
            return ValidationStatus::kInvalidMemory;
        } else if (!IsRegisterOffset(packet[2] & 0x3ffff)) {
          return ValidationStatus::kInvalidCommand;
        }
        break;
      }
      case kType3SetShReg: {
        if (packet_dwords != 3 || (packet[1] & ~0x3ffu) != 0)
          return ValidationStatus::kInvalidCommand;
        const uint32_t address = 0x2c00 + (packet[1] & 0xffff);
        if (address < 0x2c00 || address > 0x2fff) return ValidationStatus::kInvalidCommand;
        ++properties->register_writes;
        break;
      }
      case kType3SetUconfigReg: {
        if (packet_dwords != 3 || (packet[1] & ~0x3fffu) != 0)
          return ValidationStatus::kInvalidCommand;
        const uint32_t address = 0xc000 + (packet[1] & 0xffff);
        if (address < 0xc000 || address > 0xffff) return ValidationStatus::kInvalidCommand;
        ++properties->register_writes;
        break;
      }
      case kType3CopyData: {
        if (packet_dwords != 6) return ValidationStatus::kInvalidCommand;
        const uint32_t source = packet[1] & 0xf;
        const uint32_t destination = (packet[1] >> 8) & 0xf;
        const bool is_64_bit = ((packet[1] >> 16) & 1) != 0;
        if (source == 5 && (destination == 0 || destination == 4)) {
          if (!IsRegisterOffset(packet[4] & 0x3ffff))
            return ValidationStatus::kInvalidCommand;
          ++properties->register_writes;
        } else if ((source == 0 || source == 4) && (destination == 2 || destination == 5)) {
          if (!IsRegisterOffset(packet[2] & 0x3ffff))
            return ValidationStatus::kInvalidCommand;
          const uint64_t address = static_cast<uint64_t>(packet[4]) |
                                   (static_cast<uint64_t>(packet[5]) << 32);
          const uint64_t bytes = is_64_bit ? sizeof(uint64_t) : sizeof(uint32_t);
          if ((address & (bytes - 1)) != 0 || !ResolveMemory(resolve, address, bytes))
            return ValidationStatus::kInvalidMemory;
          ++properties->result_writes;
        } else {
          return ValidationStatus::kInvalidCommand;
        }
        break;
      }
      case kType3WriteData:
        if (packet_dwords < 4 || ((packet[1] >> 8) & 0xf) != 0 ||
            !IsRegisterOffset(packet[2] & 0x3ffff))
          return ValidationStatus::kInvalidCommand;
        ++properties->register_writes;
        break;
      case kType3AtomicMem: {
        if (packet_dwords != 9) return ValidationStatus::kInvalidCommand;
        const uint32_t atomic = packet[1] & 0x3f;
        const uint32_t command = (packet[1] >> 8) & 0xf;
        if (!((atomic == 8 && command == 1) || (atomic == 7 && command == 0)))
          return ValidationStatus::kInvalidCommand;
        const uint64_t address = static_cast<uint64_t>(packet[2]) |
                                 (static_cast<uint64_t>(packet[3]) << 32);
        if ((address & 0x3) != 0 || !ResolveMemory(resolve, address, sizeof(uint32_t)))
          return ValidationStatus::kInvalidMemory;
        break;
      }
      case kType3IndirectBuffer:
      default:
        return ValidationStatus::kInvalidCommand;
    }
    offset += packet_dwords;
  }

  if (offset != dword_count) return ValidationStatus::kInvalidCommand;
  if (stage == Stage::kRead && properties->result_writes == 0)
    return ValidationStatus::kInvalidCommand;
  if (stage != Stage::kRead &&
      (properties->register_writes == 0 || properties->result_writes != 0))
    return ValidationStatus::kInvalidCommand;
  return ValidationStatus::kSuccess;
}

}  // namespace detail

template <typename Resolver, typename SignalValidator>
ValidationResult ValidateRuntimePacket(const Capability& capability,
                                       const AqlProfilePacket& packet,
                                       Resolver&& resolver,
                                       SignalValidator&& validate_signal) {
  ValidationResult result{};
  if (!capability.supported) {
    result.status = ValidationStatus::kUnsupportedCapability;
    return result;
  }
  if (!IsVendorPacketHeader(packet.header) || packet.format != kAqlProfileIbFormat ||
      packet.dwords_remaining != 10 || packet.reserved[0] != kRuntimeManifestMagic ||
      packet.reserved[1] != kRuntimeManifestVersion || packet.reserved[2] == 0 ||
      packet.reserved[2] > capability.max_pm4_dwords || packet.reserved[4] != 0 ||
      packet.reserved[5] != 0 || packet.reserved[6] != 0 || packet.reserved[7] != 0) {
    result.status = ValidationStatus::kInvalidManifest;
    return result;
  }

  result.command_dwords = packet.reserved[2];
  constexpr uint32_t canonical_ib_header =
      (detail::kType3 << 30) | (2u << 16) | (detail::kType3IndirectBuffer << 8);
  constexpr uint32_t canonical_ib_controls = 1u << 23;
  const uint64_t command_address =
      (static_cast<uint64_t>(packet.ib[2]) << 32) |
      static_cast<uint64_t>(packet.ib[1] & 0xfffffffc);
  if (packet.ib[0] != canonical_ib_header ||
      (packet.ib[3] & ~0xfffffu) != canonical_ib_controls || (packet.ib[1] & 0x3) != 0 ||
      command_address == 0 || (packet.ib[3] & 0xfffff) != result.command_dwords) {
    result.status = ValidationStatus::kInvalidIndirectBuffer;
    return result;
  }

  const void* command_cpu_address = nullptr;
  const uint64_t command_bytes = static_cast<uint64_t>(result.command_dwords) * sizeof(uint32_t);
  if (!detail::ResolveMemory(resolver, command_address, command_bytes, &command_cpu_address)) {
    result.status = ValidationStatus::kInvalidCommandBuffer;
    return result;
  }
  const auto* commands = static_cast<const uint32_t*>(command_cpu_address);
  if (CommandChecksum(commands, result.command_dwords) != packet.reserved[3]) {
    result.status = ValidationStatus::kInvalidCommandBuffer;
    return result;
  }
  if (packet.completion_signal != 0 && !validate_signal(packet.completion_signal)) {
    result.status = ValidationStatus::kInvalidCompletionSignal;
    return result;
  }
  result.status = ValidationStatus::kSuccess;
  return result;
}

template <typename Resolver, typename SignalValidator>
ValidationResult ValidatePacket(const Capability& capability, const AqlProfilePacket& packet,
                                Resolver&& resolver, SignalValidator&& validate_signal) {
  ValidationResult result{};
  if (!capability.supported) {
    result.status = ValidationStatus::kUnsupportedCapability;
    return result;
  }
  if (!IsVendorPacketHeader(packet.header)) {
    result.status = ValidationStatus::kInvalidFormat;
    return result;
  }
  if (packet.format != kAqlProfileIbFormat || packet.dwords_remaining != 10) {
    result.status = ValidationStatus::kInvalidFormat;
    return result;
  }
  if (packet.reserved[0] != kManifestMagic ||
      packet.reserved[1] != kManifestVersion) {
    result.status = ValidationStatus::kInvalidManifest;
    return result;
  }

  result.stage = static_cast<Stage>(packet.reserved[2]);
  result.command_dwords = packet.reserved[3];
  result.profile_key = static_cast<uint64_t>(packet.reserved[5]) |
                       (static_cast<uint64_t>(packet.reserved[6]) << 32);
  result.event_count = packet.reserved[7] >> 8;
  const uint32_t flags = packet.reserved[7] & 0xff;
  if ((result.stage != Stage::kStart && result.stage != Stage::kRead &&
       result.stage != Stage::kStop) ||
      result.command_dwords == 0 || result.command_dwords > capability.max_pm4_dwords ||
      result.profile_key == 0 || result.event_count == 0 || flags != kManifestRequiredFlags) {
    result.status = ValidationStatus::kInvalidManifest;
    return result;
  }

  const uint32_t ib_header = packet.ib[0];
  const uint32_t ib_count = detail::PacketCount(ib_header);
  const uint32_t packet_dwords = ib_count + 2;
  const uint64_t command_address =
      (static_cast<uint64_t>(packet.ib[2]) << 32) |
      static_cast<uint64_t>(packet.ib[1] & 0xfffffffc);
  const uint32_t ib_dwords = packet.ib[3] & 0xfffff;
  constexpr uint32_t canonical_ib_header =
      (detail::kType3 << 30) | (2u << 16) | (detail::kType3IndirectBuffer << 8);
  constexpr uint32_t canonical_ib_controls = (1u << 23) | (1u << 28);
  if (ib_header != canonical_ib_header || packet_dwords != 4 ||
      (packet.ib[3] & ~0xfffffu) != canonical_ib_controls || (packet.ib[1] & 0x3) != 0 ||
      command_address == 0 || (command_address & (alignof(uint32_t) - 1)) != 0 ||
      ib_dwords != result.command_dwords) {
    result.status = ValidationStatus::kInvalidIndirectBuffer;
    return result;
  }

  const void* command_cpu_address = nullptr;
  const uint64_t command_bytes = static_cast<uint64_t>(result.command_dwords) * sizeof(uint32_t);
  if (!detail::ResolveMemory(resolver, command_address, command_bytes, &command_cpu_address)) {
    result.status = ValidationStatus::kInvalidCommandBuffer;
    return result;
  }
  const auto* commands = static_cast<const uint32_t*>(command_cpu_address);
  if (CommandChecksum(commands, result.command_dwords) != packet.reserved[4]) {
    result.status = ValidationStatus::kInvalidCommandBuffer;
    return result;
  }

  detail::StreamProperties properties{};
  result.status =
      detail::ValidateStream(commands, result.command_dwords, result.stage, resolver, &properties);
  if (result.status != ValidationStatus::kSuccess) return result;

  if (packet.completion_signal != 0 && !validate_signal(packet.completion_signal)) {
    result.status = ValidationStatus::kInvalidCompletionSignal;
    return result;
  }
  return result;
}

class SubmissionState {
 public:
  ValidationStatus Advance(Stage stage, uint64_t profile_key) {
    switch (stage) {
      case Stage::kStart:
        if (active_profile_ != 0) return ValidationStatus::kInvalidSequence;
        active_profile_ = profile_key;
        return ValidationStatus::kSuccess;
      case Stage::kRead:
        return active_profile_ == profile_key ? ValidationStatus::kSuccess
                                              : ValidationStatus::kInvalidSequence;
      case Stage::kStop:
        if (active_profile_ != profile_key) return ValidationStatus::kInvalidSequence;
        active_profile_ = 0;
        return ValidationStatus::kSuccess;
      case Stage::kNone:
      default:
        return ValidationStatus::kInvalidSequence;
    }
  }

  void Reset() { active_profile_ = 0; }
  uint64_t active_profile() const { return active_profile_; }

 private:
  uint64_t active_profile_ = 0;
};

}  // namespace profiling
}  // namespace thunk
}  // namespace wsl
