// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_CORE_SOURCE
#include "core/common/message.h"
#include "xdp/profile/database/parser/parser_utils.h"

namespace xdp {

  using severity_level = xrt_core::message::severity_level;

  MetricType getMetricTypeFromKey(const std::string& settingsKey, const std::string& key) {
    if (settingsKey == "tiles") {
      if (key == "aie_tile")        return MetricType::TILE_BASED_AIE_TILE;
      if (key == "aie")             return MetricType::TILE_BASED_CORE_MOD;
      if (key == "aie_memory")      return MetricType::TILE_BASED_MEM_MOD;
      if (key == "interface_tile")  return MetricType::TILE_BASED_INTERFACE_TILE;
      if (key == "memory_tile")     return MetricType::TILE_BASED_MEM_TILE;
      if (key == "microcontroller") return MetricType::TILE_BASED_UC;
    } else if (settingsKey == "graphs") {
      if (key == "aie_tile")        return MetricType::GRAPH_BASED_AIE_TILE;
      if (key == "aie")             return MetricType::GRAPH_BASED_CORE_MOD;
      if (key == "aie_memory")      return MetricType::GRAPH_BASED_MEM_MOD;
      if (key == "interface_tile")  return MetricType::GRAPH_BASED_INTERFACE_TILE;
      if (key == "memory_tile")     return MetricType::GRAPH_BASED_MEM_TILE;
    }
    return MetricType::NUM_TYPES;
  }

  module_type getModuleTypeFromKey(const std::string& key) {
    static const std::map<std::string, module_type> keyToModuleType = {
        {"aie",             module_type::core},
        {"aie_memory",      module_type::dma},
        {"aie_tile",        module_type::dma},        // aie_trace specific: combines core and memory functionality
        {"interface_tile",  module_type::shim},
        {"memory_tile",     module_type::mem_tile},
        {"microcontroller", module_type::uc}
    };

    auto it = keyToModuleType.find(key);
    return (it != keyToModuleType.end()) ? it->second : module_type::num_types;
  }

  bool jsonContainsRange(MetricType metricType, const boost::property_tree::ptree& jsonObj)
  {
    if ((metricType >= MetricType::TILE_BASED_AIE_TILE) &&
        (metricType < MetricType::GRAPH_BASED_AIE_TILE))
    {
      try {
        if (jsonObj.get_child_optional("start") == boost::none)
          return false;

        auto range = jsonObj.get_child_optional("start") ?
                        parseArray(jsonObj.get_child("start")) : std::vector<uint8_t>{};
        if (range.empty())
          return false;

        // NOTE: No need to check for "end" as it is optional and be same as start if not provided.
        return true;
      } catch (const boost::property_tree::ptree_error& e) {
        xrt_core::message::send(severity_level::warning, "XRT", "Error parsing start tiles in JSON: " + std::string(e.what()));
        return false;
      }
    }
    return false;
  }
  
  bool jsonContainsAllRange(MetricType metricType, const boost::property_tree::ptree& jsonObj)
  {
    if ((metricType >= MetricType::TILE_BASED_AIE_TILE) &&
        (metricType < MetricType::GRAPH_BASED_AIE_TILE))
    {
      try {
        auto allTiles = jsonObj.get_optional<bool>("all_tiles");
        if (allTiles && *allTiles)
          return true;
      } catch (const boost::property_tree::ptree_error& e) {
        xrt_core::message::send(severity_level::warning, "XRT", "Error parsing all_tiles schema in JSON: " + std::string(e.what()));
        return false;
      }
    }
    else if (metricType >= MetricType::GRAPH_BASED_AIE_TILE &&
             metricType <= MetricType::GRAPH_BASED_MEM_TILE)
    {
      try {
        auto allGraphs = jsonObj.get_optional<std::string>("graph");
        if (allGraphs && *allGraphs == "all")
          return true;
      } catch (const boost::property_tree::ptree_error& e) {
        xrt_core::message::send(severity_level::warning, "XRT", "Error parsing all graph schema in JSON: " + std::string(e.what()));
        return false;
      }
    }
    return false;
  }
}
