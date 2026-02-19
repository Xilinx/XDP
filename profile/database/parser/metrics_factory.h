// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved
#ifndef XDP_METRICS_FACTORY_H
#define XDP_METRICS_FACTORY_H

#include "metrics_type.h"
#include "metrics.h"

namespace xdp
{

  class MetricsFactory 
  {
    public:
      static std::unique_ptr<Metric> createMetric(MetricType type, const boost::property_tree::ptree& obj) {
        switch (type) {
            case MetricType::TILE_BASED_AIE_TILE:
            case MetricType::TILE_BASED_CORE_MOD:
            case MetricType::TILE_BASED_MEM_MOD:
            case MetricType::TILE_BASED_INTERFACE_TILE:
            case MetricType::TILE_BASED_MEM_TILE:
            case MetricType::TILE_BASED_UC:
                return TileBasedMetricEntry::processSettings(type, obj);
            case MetricType::GRAPH_BASED_AIE_TILE:
            case MetricType::GRAPH_BASED_CORE_MOD:
            case MetricType::GRAPH_BASED_MEM_MOD:
            case MetricType::GRAPH_BASED_INTERFACE_TILE:
            case MetricType::GRAPH_BASED_MEM_TILE:
                return GraphBasedMetricEntry::processSettings(type, obj);
            default:
                xrt_core::message::send(
                    severity_level::warning,
                    "XRT",
                    "Unknown or unsupported MetricType (" + std::to_string(static_cast<int>(type)) + ")");
                return nullptr;
        }
      }
  };
}; // namespace xdp

#endif
