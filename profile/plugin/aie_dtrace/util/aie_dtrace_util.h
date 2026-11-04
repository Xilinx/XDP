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

  // One PLIO controller entry parsed from plip_info.json.
  struct PlioInfoPort {
    uint8_t column;    // Shim (interface tile) column that hosts the PLIO controller
    uint8_t channel;   // south<N> port number (channel 0 == south0, channel 1 == south1, ...)
  };

  // True for the plio_read_bandwidth metric set.
  bool isPlioBandwidthMetric(const std::string& metricSet);

  // One task jprobe for a control program: the asm line that generates PL User
  // Event 1, and the clock-counter stop threshold in effect at that line.
  struct PlWtsProbe {
    int line = 0;
    uint32_t stopCount = 0;
  };

  // Parse pl_wts.json from the current working directory.
  // File keys are "0", "1", "2", ... and map to jprobe UC numbers 0, 4, 8, ...
  // (key i -> UC i * 4). Each entry's pl_wts_config_lines[k] is paired with
  // pl_wts_data_bytes[k]. The stop count at task k is the integer ceiling of
  // the sum of byte counts through k, divided by 8. A missing file, a
  // mismatched pair of lists, or a non-integer field drops that UC and logs
  // a warning. The returned map is keyed by jprobe UC number.
  std::map<int, std::vector<PlWtsProbe>> parsePlWtsJson();

  // AIE2PS shim stream-switch port index for a south<channel> PLIO port.
  // Per the AIE2 shim slave/master port table, south0 maps to physical port 2,
  // south1 to 3, etc. (tile_control_rsp=0, fifo=1 occupy the first two slots).
  uint8_t getPlioSouthStreamPortIndex(uint8_t channel);

  // Parse the PLIO overlay metadata (plip_info.json) and return the available
  // PLIO controller ports (column + south channel). The file is read from the
  // current working directory.
  std::vector<PlioInfoPort> parsePlioInfoJson();

  // ============================ Partition geometry ===========================

  // The columns one hardware context owns.
  struct PartitionGeometry {
    bool valid = false;
    uint8_t startCol = 0;
    uint32_t numCols = 0;
  };

  // Resolves the partition belonging to this hardware context. The underlying
  // aie_partition_info query is device-wide, so it also reports partitions of
  // other contexts and other processes; picking the wrong one puts every
  // counter address in the CT file in somebody else's columns.
  PartitionGeometry getPartitionGeometry(void* hwctx);

  // ===========================L2L2 transfer metrics ==========================================

  // Inter-stamp memtile halo dst paths; design points come from xrt.ini.
  // Max dst halo paths per memtile column (4 perf counters, running+stalled per path).
  static constexpr uint8_t L2L2_MAX_DST_PATHS_PER_COLUMN = 2;
  // Memtile row 1 (absolute array row 1; row 0 = shim/interface tile).
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

  // Parses memory_tile_input_ports, e.g. "{1,1:2},{5,1:1},{5,1:2}".
  // Column is partition-relative (0 = partition start_col); row is ignored.
  std::vector<L2L2InstrumentPoint> parseL2L2DesignPoints(const std::string& spec);

  // Builds running+stalled counter pairs from design points within the partition.
  // Columns in instrumentPoints are partition-relative (0 .. numCols-1).
  std::vector<L2L2CounterPoint> getL2L2CounterPoints(
      uint32_t numCols,
      const std::vector<L2L2InstrumentPoint>& instrumentPoints);

  // =========================== Memtile memory conflicts =====================================

  // Group_Memory_Conflict (event 111); counted on memtile perf counter 4 so L2-L2
  // (counters 0-3) can run on the same tile. Which banks feed the event is selected
  // by Event_Group_Memory_Conflict_Enable, which has one bit per bank (8 banks).
  static constexpr uint8_t MEMORY_CONFLICT_COUNTER = 4;
  static constexpr uint8_t GROUP_MEMORY_CONFLICT_EVENT = 111;
  static constexpr uint8_t NUM_MEMTILE_BANKS = 8;
  static constexpr uint8_t ALL_MEMTILE_BANKS = 0xFF;

  struct MemoryConflictTile {
    uint8_t column = 0;  // partition-relative
    uint8_t row = 0;     // absolute array row (required)
    uint8_t bank = ALL_MEMTILE_BANKS;  // 0-7, or ALL_MEMTILE_BANKS for every bank
    bool allTiles = false;  // {all:bank}: every memtile in the partition (column/row unused)
  };

  // Parses memory_tile_conflict_points, e.g. "{1,1:all},{5,1:3},{9,1:}" or "{all:0}".
  // Column is partition-relative; row selects the exact memtile. Bank "all" or empty
  // monitors every bank, 0-7 monitors that bank only; any other bank value is skipped.
  // Invalid entries are ignored. The first valid {all:bank} entry is returned alone:
  // entries before it are dropped and entries after it are not read. Otherwise only the
  // first entry for a tile is kept. Each dropped entry is reported in droppedEntries
  // (if given) as "<dropped entry> (kept <kept entry>)".
  std::vector<MemoryConflictTile> parseMemoryConflictPoints(
      const std::string& spec,
      std::vector<std::string>* droppedEntries = nullptr);

  // Keeps tiles with column in [0, numCols) and row in validMemRows (if non-empty).
  // An allTiles entry expands to every column in [0, numCols) on every row in validMemRows.
  std::vector<MemoryConflictTile> filterMemoryConflictTiles(
      uint32_t numCols,
      const std::vector<uint8_t>& validMemRows,
      const std::vector<MemoryConflictTile>& tiles);

  // ========================================================================================
  // Apply JSON + coalesced dtrace_dump defaults when those keys are absent.
  // Values already present in xrt.ini or the environment are left unchanged.
  // Must run before XRT creates the first dtrace module (config keys lock on first read).
  void initDtraceOutputConfig();

} // namespace xdp::aie::dtrace

#endif
