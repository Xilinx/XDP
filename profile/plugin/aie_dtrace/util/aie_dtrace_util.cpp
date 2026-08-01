// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/util/aie_dtrace_util.h"

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

  std::vector<L2L2CounterPoint> getL2L2CounterPoints(uint32_t numCols)
  {
    if (numCols != L2L2_BASELINE_NUM_COLS)
      return {};

    std::vector<L2L2CounterPoint> points;
    points.reserve(L2L2_BASELINE_NUM_COUNTERS);

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

    return points;
  }

} // namespace xdp::aie::dtrace
