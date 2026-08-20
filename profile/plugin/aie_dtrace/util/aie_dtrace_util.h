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

  // Inter-stamp memtile halo dst paths; design points come from xrt.ini.
  // Max dst halo paths per memtile column (4 perf counters, running+stalled per path).
  static constexpr uint8_t L2L2_MAX_DST_PATHS_PER_COLUMN = 2;
  // Memtile row 0 (absolute array row 1; row 0 = shim/interface tile).
  static constexpr uint8_t MEM_TILE_ROW_START = 1;

  struct L2L2InstrumentPoint {
    uint8_t column = 0;
    uint8_t dstPort = 1;
  };

  // One perf counter at a memtile dst halo path (running or stalled).
  struct L2L2CounterPoint {
    uint8_t column = 0;
    uint8_t row = 0;
    uint8_t portIndex = 1;       // halo dst port (1 = from left neighbor, 2 = from right)
    uint8_t counterNumber = 0;   // memtile perf counter 0-3 on this tile
    std::string eventType;         // "running" or "stalled"
  };

  // Parses AIE_dtrace_settings.l2_l2_design_points, e.g. "{1,1:2},{5,1:1},{5,1:2}".
  // INI uses {column,row:port} for readability; row is ignored — counters use MEM_TILE_ROW_START.
  std::vector<L2L2InstrumentPoint> parseL2L2DesignPoints(const std::string& spec);

  // Builds running+stalled counter pairs from xrt.ini design points within the partition.
  std::vector<L2L2CounterPoint> getL2L2CounterPoints(
      uint32_t startCol,
      uint32_t numCols,
      const std::vector<L2L2InstrumentPoint>& instrumentPoints);

  // ========================================================================================

  // Enable JSON dtrace_dump output with coalesced results by default.
  // Must run before XRT creates the first dtrace module (config keys lock on first read).
  void initDtraceOutputConfig();

} // namespace xdp::aie::dtrace

#endif
