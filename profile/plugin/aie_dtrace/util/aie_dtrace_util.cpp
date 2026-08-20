// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/util/aie_dtrace_util.h"

#include "core/common/config_reader.h"
#include "core/common/message.h"

#include <map>
#include <mutex>
#include <regex>

namespace xdp::aie::dtrace {

  namespace {
    using severity_level = xrt_core::message::severity_level;

    static constexpr unsigned int DEFAULT_COALESCE_RESULT_MEMORY_MB = 256;

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

  void
  initDtraceOutputConfig()
  {
    static std::once_flag once;
    std::call_once(once, []() {
      try {
        xrt_core::config::detail::set("Debug.dtrace_output_json_format", "true");
        xrt_core::config::detail::set("Debug.dtrace_coalesce_result", "true");
        xrt_core::config::detail::set("Debug.dtrace_coalesce_result_memory_mb",
                                      std::to_string(DEFAULT_COALESCE_RESULT_MEMORY_MB));
      }
      catch (const std::exception& e) {
        xrt_core::message::send(severity_level::warning, "XRT",
            std::string("AIE dtrace: could not apply default dtrace output settings: ")
            + e.what());
        return;
      }

      xrt_core::message::send(severity_level::info, "XRT",
          "AIE dtrace: enabled JSON dtrace_dump output with coalesced results "
          "(dtrace_dump_ctx_<slot>_<timestamp>.json on hw context teardown)");
    });
  }

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

    // Format: {column,row:port} — row is accepted for INI readability only (ignored).
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

    const uint32_t endCol = startCol + numCols;
    std::vector<L2L2CounterPoint> points;
    points.reserve(instrumentPoints.size() * 2);

/*=================old hardcoded values==========================
// Stamp 0 edge (col 1): one dst path -> ctr 0,1 -> PerfCtrl0 only
addPortCounterPair(points, 1, 2, 0, 1);

// Stamps 1-4 middle (cols 5, 9, 13, 17): two dst paths -> ctr 0-3 -> PerfCtrl0 + PerfCtrl1
addPortCounterPair(points, 5,  1, 0, 1);
addPortCounterPair(points, 5,  2, 2, 3);
addPortCounterPair(points, 9,  1, 0, 1);
addPortCounterPair(points, 9,  2, 2, 3);
addPortCounterPair(points, 13, 1, 0, 1);
addPortCounterPair(points, 13, 2, 2, 3);
addPortCounterPair(points, 17, 1, 0, 1);
addPortCounterPair(points, 17, 2, 2, 3);
// Stamp 5 edge (col 21): one dst path -> ctr 0,1 -> PerfCtrl0 only
addPortCounterPair(points, 21, 1, 0, 1);
===============================================================  */

    // Assign counters 0-1 for the first dst path on a tile, 2-3 for the second.
    std::map<uint8_t, uint8_t> nextCounterByColumn;
    for (const auto& instrumentPoint : instrumentPoints) {
      const uint32_t column = instrumentPoint.column;
      if (column < startCol || column >= endCol)
        continue;

      uint8_t& nextCounter = nextCounterByColumn[instrumentPoint.column];
      if (nextCounter >= L2L2_MAX_DST_PATHS_PER_COLUMN * 2)
        continue;

      addPortCounterPair(points, instrumentPoint.column, instrumentPoint.dstPort,
                         nextCounter, static_cast<uint8_t>(nextCounter + 1));
      nextCounter = static_cast<uint8_t>(nextCounter + 2);
    }

    return points;
  }

} // namespace xdp::aie::dtrace
