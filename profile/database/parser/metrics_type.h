// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved
#ifndef XDP_METRICS_TYPE_H
#define XDP_METRICS_TYPE_H

namespace  xdp {
  // NOTE: TILE_BASED and GRAPH_BASED needs to be grouped together
  //       as they are used to determine the type of metric in the parser.
  enum class MetricType {
      TILE_BASED_AIE_TILE = 0,
      TILE_BASED_CORE_MOD,
      TILE_BASED_MEM_MOD,
      TILE_BASED_INTERFACE_TILE,
      TILE_BASED_MEM_TILE,
      TILE_BASED_UC,
      GRAPH_BASED_AIE_TILE,
      GRAPH_BASED_CORE_MOD,
      GRAPH_BASED_MEM_MOD,
      GRAPH_BASED_INTERFACE_TILE,
      GRAPH_BASED_MEM_TILE,
      NUM_TYPES // Used to determine the number of metric types
  };
};
#endif