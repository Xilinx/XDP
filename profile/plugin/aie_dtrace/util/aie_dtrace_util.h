// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#ifndef AIE_DTRACE_UTIL_DOT_H
#define AIE_DTRACE_UTIL_DOT_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include <aie_codegen.h>
}

namespace xdp::aie::dtrace {

  // Shim bandwidth metric sets used for Debug.aie_dtrace (not part of standard aie_profile ini).
  std::map<std::string, std::vector<XAie_Events>> getBandwidthInterfaceTileEventSets(int hwGen);

  // ===========================L2L2 transfer metrics ==========================================

  // Fixed baseline overlay: 1x6x4x4 (6 stamps, 4 cols/stamp, 24 partition columns).
  // Inter-stamp memtile halo dst paths only (AIESW-27919 Type B).
  static constexpr uint32_t L2L2_BASELINE_NUM_COLS = 24;
  static constexpr uint32_t L2L2_BASELINE_NUM_COUNTERS = 20;
  // AIE memtile starting row index on the 1x6x4x4 baseline (row 0 = shim/interface tile).
  static constexpr uint8_t MEM_TILE_ROW_START = 1;

  // One perf counter at a memtile dst halo path (running or stalled).
  struct L2L2CounterPoint {
    uint8_t column = 0;
    uint8_t row = 0;
    uint8_t portIndex = 1;       // halo dst port (1 = from left neighbor, 2 = from right)
    uint8_t counterNumber = 0;   // memtile perf counter 0-3 on this tile
    std::string eventType;         // "running" or "stalled"
  };

  // Returns the fixed 20-counter baseline table when numCols == L2L2_BASELINE_NUM_COLS; else empty.
  std::vector<L2L2CounterPoint> getL2L2CounterPoints(uint32_t numCols);

  // ========================================================================================

} // namespace xdp::aie::dtrace

#endif
