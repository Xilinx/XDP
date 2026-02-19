// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved
#include "metrics_collection_manager.h"

namespace xdp
{
    void MetricsCollectionManager::addMetricCollection(module_type mod, const std::string& settingName, MetricCollection collection)
    {
      std::cout << "!!! Adding metric collection for module: " << static_cast<int>(mod) << ", setting: " << settingName << std::endl;
      allModulesMetricCollections[mod][settingName] = std::move(collection);

      // Make sure it was added
      auto modIt = allModulesMetricCollections.find(mod);
      if (modIt != allModulesMetricCollections.end())
      {
        auto settingIt = modIt->second.find(settingName);
        if (settingIt != modIt->second.end())
        {
          std::cout << "!! Metric collection added successfully." << std::endl;
        }
        else
        {
          std::cout << "!! ERROR: Failed to add metric collection. - setting" << std::endl;
        }
      }
      else
      {
        std::cout << "!! ERROR: Failed to add metric collection - Mod" << std::endl;
      }
    }

    const MetricCollection& MetricsCollectionManager::getMetricCollection(module_type mod, const std::string& settingName) const
    {
      std::cout << "!! Getting metric collection for module: " << static_cast<int>(mod) << ", setting: " << settingName << std::endl;
      auto modIt = allModulesMetricCollections.find(mod);
      if (modIt != allModulesMetricCollections.end())
      {
        // std::cout << "!! Found module: " << static_cast<int>(mod) << std::endl;
        auto settingIt = modIt->second.find(settingName);
        if (settingIt != modIt->second.end())
        {
          return settingIt->second;
        }
        std::cout << "!! ERROR: Setting not found: " << settingName << std::endl;
      }

      static const MetricCollection emptyCollection;
      return emptyCollection; // Return an empty collection if not found
    }

    void MetricsCollectionManager::print() const
    {
      for (const auto& module : allModulesMetricCollections) {
        std::cout << "Module: " << static_cast<int>(module.first) << std::endl;
        for (const auto& collection : module.second) {
            std::cout << "  Setting: " << collection.first << std::endl;
            collection.second.print();
        }
      }
    }

}; // namespace xdp
