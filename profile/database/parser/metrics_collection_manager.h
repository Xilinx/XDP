// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved
#ifndef XDP_METRICS_COLLECTION_MANAGER_H
#define XDP_METRICS_COLLECTION_MANAGER_H
#include <map>
#include "metrics.h"
#include "metrics_collection.h"

namespace xdp
{
  class MetricsCollectionManager
  {
  private:
    // module_type , module_type_string -> MetricCollection
    // TODO: module_type_string (aie, aie_memory, interface_tile, memory_tile, etc.) is used.
    // This can be extended to separate out "tiles" and "graphs" if needed.
    std::map<module_type, std::map<std::string, MetricCollection>> allModulesMetricCollections;
  public:
    MetricsCollectionManager() = default;
    ~MetricsCollectionManager() = default;

    // Methods to manage metrics collections
    void addMetricCollection(module_type mod, const std::string& settingName, MetricCollection collection);
    const MetricCollection& getMetricCollection(module_type mod, const std::string& settingName) const;
    void print() const;
  };
}; // namespace xdp
#endif