// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved
#include "metrics_collection_manager.h"

namespace xdp
{
    void MetricsCollectionManager::addMetricCollection(module_type mod, const std::string& settingName, MetricCollection collection)
    {
      allModulesMetricCollections[mod][settingName] = std::move(collection);
    }

    const MetricCollection& MetricsCollectionManager::getMetricCollection(module_type mod, const std::string& settingName) const
    {
      auto modIt = allModulesMetricCollections.find(mod);
      if (modIt != allModulesMetricCollections.end())
      {
        auto settingIt = modIt->second.find(settingName);
        if (settingIt != modIt->second.end())
        {
          return settingIt->second;
        }
      }

      static const MetricCollection emptyCollection;
      return emptyCollection; // Return an empty collection if not found
    }

    void MetricsCollectionManager::print() const
    {
    }

}; // namespace xdp
