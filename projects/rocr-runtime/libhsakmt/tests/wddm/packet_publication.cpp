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

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "core/util/atomic_helpers.h"

namespace {

constexpr uint16_t kInvalidHeader = 0x0001;
constexpr uint16_t kValidHeader = 0x1202;
constexpr uint32_t kIterations = 200000;

struct alignas(64) Packet {
  uint16_t header;
  uint16_t setup;
  uint32_t body[15];
};
static_assert(sizeof(Packet) == 64);

uint32_t BodyValue(uint32_t iteration, uint32_t index) {
  return 0x9e3779b9u * (iteration + 1) ^ (0x01010101u * index);
}

}  // namespace

int main() {
  Packet packet{};
  packet.header = kInvalidHeader;
  std::atomic<uint32_t> failures{0};

  std::thread producer([&]() {
    for (uint32_t iteration = 0; iteration < kIterations; ++iteration) {
      while (rocr::atomic::Load(&packet.header, std::memory_order_acquire) != kInvalidHeader) {
        std::this_thread::yield();
      }

      packet.setup = static_cast<uint16_t>(1 + (iteration % 3));
      for (uint32_t index = 0; index < 15; ++index) {
        packet.body[index] = BodyValue(iteration, index);
      }
      rocr::atomic::Store(&packet.header, kValidHeader, std::memory_order_release);
    }
  });

  std::thread consumer([&]() {
    for (uint32_t iteration = 0; iteration < kIterations; ++iteration) {
      while (rocr::atomic::Load(&packet.header, std::memory_order_acquire) != kValidHeader) {
        std::this_thread::yield();
      }

      if (packet.setup != static_cast<uint16_t>(1 + (iteration % 3))) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
      for (uint32_t index = 0; index < 15; ++index) {
        if (packet.body[index] != BodyValue(iteration, index)) {
          failures.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }
      rocr::atomic::Store(&packet.header, kInvalidHeader, std::memory_order_release);
    }
  });

  producer.join();
  consumer.join();

  const auto failure_count = failures.load(std::memory_order_relaxed);
  std::printf("packet publication iterations=%u failures=%u final_header=0x%04x final_setup=%u\n",
              kIterations, failure_count, static_cast<unsigned>(packet.header),
              static_cast<unsigned>(packet.setup));
  return failure_count == 0 ? 0 : 1;
}
