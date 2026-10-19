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

// The qualified native-WDDM AQL Profile software queue. The frame has to hold the
// aqlprofile-built PM4 indirect buffer plus the completion trailer. Profile packets
// remain inlined within this budget; a retained PM4 command list that does not fit a
// frame can use the variable-size command-buffer path advertised by version 2.
constexpr uint32_t kMinimumFrameBytes = 0x500;
constexpr uint32_t kQualifiedFrameBytes = 0x2000;
constexpr uint32_t kFrameTrailerReserveBytes = 0x100;
constexpr uint32_t kMaximumPm4Dwords =
    (kQualifiedFrameBytes - kFrameTrailerReserveBytes) / sizeof(uint32_t);
// IT_INDIRECT_BUFFER encodes the stream length in a 20-bit field. Profile
// packets remain bounded by the frame budget above; a retained PM4 command
// list that does not fit a frame is submitted from its own variable-size
// command buffer, so only the hardware field bounds it.
constexpr uint32_t kMaximumIbDwords = 0xfffffu;

// Describes the AQL Profile PM4 indirect-buffer capability of a device.
struct Capability {
  bool supported = false;
  uint32_t version = 0;
  uint32_t max_pm4_dwords = 0;
};

constexpr Capability DetectCapability(uint32_t major, uint32_t minor, uint32_t stepping,
                                      uint32_t frame_bytes, bool software_queue_available) {
  // Native WDDM profiling is qualified only for gfx1151. Additional targets must
  // define and validate their packet policy before joining this capability set.
  const bool gfx1151 = major == 11 && minor == 5 && stepping == 1;
  const bool frame_is_sufficient = frame_bytes >= kMinimumFrameBytes;
  // Vendor PM4 is translated by the software AQL-to-PM4 queue. Do not
  // advertise the capability on a queue that will submit native AQL: the
  // AQL Profile packet would not have a translation path.
  const bool supported = gfx1151 && frame_is_sufficient && software_queue_available;
  const uint32_t available_dwords =
      supported ? (frame_bytes - kFrameTrailerReserveBytes) / sizeof(uint32_t) : 0u;
  // Version 2 additionally advertises that the software queue can submit a
  // retained PM4 stream larger than one frame from a variable-size command
  // buffer. Version 1 thunks only support the inlined frame budget.
  return {supported,
          supported ? 2u : 0u,
          available_dwords < kMaximumPm4Dwords ? available_dwords : kMaximumPm4Dwords};
}

}  // namespace profiling
}  // namespace thunk
}  // namespace wsl
