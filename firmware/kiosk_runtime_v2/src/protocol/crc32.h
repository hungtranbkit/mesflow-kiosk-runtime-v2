// Plain C++, no Arduino.h — host-testable. Groundwork for the Phase 3
// durable journal's per-record CRC (docs/OFFLINE.md); not wired into any
// journal yet since the journal itself doesn't exist in Phase 0.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kiosk::protocol {

// Standard CRC-32 (IEEE 802.3 polynomial 0xEDB88320), computed bitwise —
// no static lookup table, so this header has no .cpp companion and no
// startup cost. Fine for journal-record-sized buffers; revisit if profiling
// ever shows this hot.
inline uint32_t crc32(const uint8_t* data, size_t length, uint32_t seed = 0xFFFFFFFFu) {
  uint32_t crc = seed;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

}  // namespace kiosk::protocol
