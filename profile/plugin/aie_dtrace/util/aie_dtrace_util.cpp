// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/util/aie_dtrace_util.h"

#include <map>
#include <regex>

namespace xdp::aie::dtrace {

  namespace {

    void addPortCounterPair(std::vector<L2L2CounterPoint>& points,
                            uint8_t column,
                            uint8_t portIndex,
                            uint8_t runningCounter,
                            uint8_t stalledCounter)
    {
      L2L2CounterPoint running;
      running.column = column;
      running.row = MEM_TILE_ROW_START;
      running.portIndex = portIndex;
      running.counterNumber = runningCounter;
      running.eventType = "running";
      points.push_back(running);

      L2L2CounterPoint stalled;
      stalled.column = column;
      stalled.row = MEM_TILE_ROW_START;
      stalled.portIndex = portIndex;
      stalled.counterNumber = stalledCounter;
      stalled.eventType = "stalled";
      points.push_back(stalled);
    }

  } // namespace

  std::map<std::string, std::vector<XAie_Events>>
  getBandwidthInterfaceTileEventSets(int hwGen)
  {
    (void)hwGen;
    return {
      {"read_bandwidth", {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_RUNNING_1_PL}},
      {"write_bandwidth", {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_RUNNING_1_PL}},
      {"ddr_bandwidth",
       {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_RUNNING_1_PL, XAIE_EVENT_PORT_RUNNING_2_PL,
        XAIE_EVENT_PORT_RUNNING_3_PL}},
      {"peak_read_bandwidth",
       {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_STALLED_0_PL,
        XAIE_EVENT_PORT_RUNNING_1_PL, XAIE_EVENT_PORT_STALLED_1_PL}},
      {"peak_write_bandwidth",
       {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_STALLED_0_PL,
        XAIE_EVENT_PORT_RUNNING_1_PL, XAIE_EVENT_PORT_STALLED_1_PL}},
    };
  }

  std::vector<L2L2InstrumentPoint> parseL2L2DesignPoints(const std::string& spec)
  {
    std::vector<L2L2InstrumentPoint> points;
    if (spec.empty())
      return points;

    // Format: {column,row:port} — column is partition-relative (0 = partition
    // start_col); row is accepted for readability only (ignored).
    static const std::regex pointRegex(R"(\{\s*(\d+)\s*,\s*(\d+)\s*:\s*(\d+)\s*\})");
    const auto begin = std::sregex_iterator(spec.begin(), spec.end(), pointRegex);
    const auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
      try {
        const unsigned long column = std::stoul((*it)[1].str());
        const unsigned long dstPort = std::stoul((*it)[3].str());
        if (column > 255 || (dstPort != 1 && dstPort != 2))
          continue;

        L2L2InstrumentPoint point;
        point.column = static_cast<uint8_t>(column);
        point.dstPort = static_cast<uint8_t>(dstPort);
        points.push_back(point);
      }
      catch (const std::exception&) {
        continue;
      }
    }
    return points;
  }

  std::vector<L2L2CounterPoint> getL2L2CounterPoints(
      uint32_t startCol,
      uint32_t numCols,
      const std::vector<L2L2InstrumentPoint>& instrumentPoints)
  {
    if (numCols == 0 || instrumentPoints.empty())
      return {};

    // Design-point columns are partition-relative (0 .. numCols-1). Counters and
    // CT addresses use the same relative column as the rest of the CT writer.
    std::vector<L2L2CounterPoint> points;
    points.reserve(instrumentPoints.size() * 2);

    // Assign counters 0-1 for the first dst path on a tile, 2-3 for the second.
    std::map<uint8_t, uint8_t> nextCounterByColumn;
    for (const auto& instrumentPoint : instrumentPoints) {
      const uint32_t column = instrumentPoint.column;
      if (column >= numCols)
        continue;

      uint8_t& nextCounter = nextCounterByColumn[instrumentPoint.column];
      if (nextCounter >= L2L2_MAX_DST_PATHS_PER_COLUMN * 2)
        continue;

      addPortCounterPair(points, instrumentPoint.column, instrumentPoint.dstPort,
                         nextCounter, static_cast<uint8_t>(nextCounter + 1));
      nextCounter = static_cast<uint8_t>(nextCounter + 2);
    }

    (void)startCol;
    return points;
  }

} // namespace xdp::aie::dtrace
