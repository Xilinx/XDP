// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "aie_profile_metadata.h"

#include <boost/algorithm/string.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "core/common/config_reader.h"
#include "core/common/device.h"
#include "core/common/message.h"
#include "xdp/profile/database/database.h"
#include "xdp/profile/plugin/vp_base/vp_base_plugin.h"
#include "xdp/profile/database/parser/metrics.h"

namespace xdp {
  using severity_level = xrt_core::message::severity_level;
  namespace pt = boost::property_tree;

  void AieProfileMetadata::getConfigMetricsUsingJson(const int module, const module_type type,
                                  MetricsCollectionManager& metricsCollectionManager)
  {
    if (type == module_type::shim)
        getConfigMetricsForInterfaceTilesUsingJson(module, metricsCollectionManager);
    else if (type == module_type::uc)
        getConfigMetricsForMicrocontrollersUsingJson(module, metricsCollectionManager);
    else
        getConfigMetricsForTilesUsingJson(module, type, metricsCollectionManager);
  }

  /****************************************************************************
   * Resolve metrics for AIE or Memory tiles
   ***************************************************************************/
  void AieProfileMetadata::populateGraphConfigMetricsForTilesUsingJson(const int moduleIdx, 
      const module_type mod, MetricsCollectionManager& metricsCollectionManager)
  {
    std::string metricSettingsName = moduleNames[moduleIdx];
    // uint8_t rowOffset     = (mod == module_type::mem_tile) ? 1 : metadataReader->getAIETileRowOffset();
    std::string entryName = (mod == module_type::mem_tile) ? "buffer" : "kernel";
    std::string modName   = (mod == module_type::core) ? "aie" 
                          : ((mod == module_type::dma) ? "aie_memory" : "memory_tile");

    auto allValidGraphs  = metadataReader->getValidGraphs();
    std::vector<std::string> allValidEntries = (mod == module_type::mem_tile) ?
      metadataReader->getValidBuffers() : metadataReader->getValidKernels();

    std::set<tile_type> allValidTiles;
    auto validTilesVec = metadataReader->getTiles("all", mod, "all");
    std::unique_copy(validTilesVec.begin(), validTilesVec.end(), std::inserter(allValidTiles, allValidTiles.end()),
                     xdp::aie::tileCompare);
    const MetricCollection& tilesMetricCollection = metricsCollectionManager.getMetricCollection(mod, metricSettingsName);
    const auto& metrics = tilesMetricCollection.metrics;


    // Parse per-graph or per-kernel settings

    /*
     * Example JSON config format for AI Engine Tiles:
     *
     * {
     *   "graphs": {
     *     "aie": [
     *       {
     *         "graph": "<graph name|all>",
     *         "kernel": "<kernel name|all>",
     *         "metric": "<off|heat_map|stalls|execution|floating_point|write_throughputs|read_throughputs|aie_trace>"
     *       }
     *     ],
     *     "aie_memory": [
     *       {
     *         "graph": "<graph name|all>",
     *         "kernel": "<kernel name|all>",
     *         "metric": "<off|conflicts|dma_locks|dma_stalls_s2mm|dma_stalls_mm2s|write_throughputs|read_throughputs>"
     *       }
     *     ]
     *   }
     * }
     *
     * For Memory Tiles (AIE2 and beyond):
     * {
     *   "graphs": {
     *     "memory_tile": [
     *       {
     *         "graph": "<graph name|all>",
     *         "buffer": "<buffer name|all>",
     *         "metric": "<off|input_channels|input_channels_details|output_channels|output_channels_details|memory_stats|mem_trace>",
     *         "channels": [<optional channel numbers>]
     *       }
     *     ]
     *   }
     * }
     */

    // Only one graphs setting type is supported at a time in JSON.
    // Step 1a: Process all graphs metric setting ("graph": "all")
    // Step 1b: Process single graph metric setting ("graph": "<graph name>")



    bool allGraphs = false;
    // Step 1a: Process all graphs metric setting ( "all_graphs" )
    for (size_t i = 0; i < metrics.size(); ++i) {

      if (!metrics[i]->isGraphBased()) {
        xrt_core::message::send(severity_level::warning, "XRT",
                                "JSON Settings: Skipping metric " + metrics[i]->getMetric() + 
                                " as it is not graph-based for " + modName + " module.");
        continue;
      }

      // Check if graph is not all or if invalid kernel
      if (!metrics[i]->isAllTilesSet())
        continue;
      
      // Check if all graphs setting is already processed
      if (allGraphs)
        break;

      std::string graphName = metrics[i]->getGraph();
      std::string graphEntity = metrics[i]->getGraphEntity();
      if ((graphEntity != "all") &&
          (std::find(allValidEntries.begin(), allValidEntries.end(), graphEntity) == allValidEntries.end())) {
        std::stringstream msg;
        msg << "Could not find " << entryName << " " << graphEntity
            << " as specified in aie_profile.graphs." << modName << " setting."
            << " The following " << entryName << "s are valid : " << allValidEntries[0];

        for (size_t j = 1; j < allValidEntries.size(); j++)
          msg << ", " << allValidEntries[j];

        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        continue;
      }

      auto tiles = metadataReader->getTiles(graphName, mod, graphEntity);
      for (auto& e : tiles) {
        configMetrics[moduleIdx][e] = metrics[i]->getMetric();
      }

      // Grab channel numbers (if specified; memory tiles only)
      if ((metrics[i]->isChannel0Set()) && (metrics[i]->isChannel1Set())) {
        try {
          for (auto& e : tiles) {
            configChannel0[e] = metrics[i]->getChannel0();
            configChannel1[e] = metrics[i]->getChannel1();
          }
        }
        catch (...) {
          std::stringstream msg;
          msg << "Channel specifications in aie_profile.graphs." << modName << " are not valid and hence ignored.";
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        }
      }
      allGraphs = true;
    } // Graph Pass 1a

    // Step 1b: Process single graph metric setting
    for (size_t i = 0; i < metrics.size(); ++i) {
      // Check if already processed or if invalid
      if (allGraphs)
        break;
      if (!metrics[i]->isGraphBased()) {
        xrt_core::message::send(severity_level::warning, "XRT",
                                "JSON Settings: Skipping metric " + metrics[i]->getMetric() + 
                                " as it is not graph-based for " + modName + " module.");
        continue;
      }

      const std::string& graphName = metrics[i]->getGraph();
      const std::string& graphEntity = metrics[i]->getGraphEntity();
      
      // Check if graph name is valid (when not "all")
      if ((graphName != "all") && 
          (std::find(allValidGraphs.begin(), allValidGraphs.end(), graphName) == allValidGraphs.end())) {
        std::stringstream msg;
        msg << "Could not find graph " << graphName
            << " as specified in aie_profile.graphs." << modName << " setting."
            << " The following graphs are valid : " << allValidGraphs[0];

        for (size_t j = 1; j < allValidGraphs.size(); j++)
          msg << ", " << allValidGraphs[j];

        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        continue;
      }
      
      if ((graphEntity != "all") &&
          (std::find(allValidEntries.begin(), allValidEntries.end(), graphEntity) == allValidEntries.end())) {
        std::stringstream msg;
        msg << "Could not find " << entryName << " " << graphEntity
            << " as specified in aie_profile.graphs." << modName << " setting."
            << " The following " << entryName << "s are valid : " << allValidEntries[0];

        for (size_t j = 1; j < allValidEntries.size(); j++)
          msg << ", " << allValidEntries[j];

        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        continue;
      }

      // Capture all tiles in given graph
      auto tiles = metadataReader->getTiles(graphName, mod, graphEntity);
      for (auto& e : tiles) {
        configMetrics[moduleIdx][e] = metrics[i]->getMetric();
      }

      // Grab channel numbers (if specified; memory tiles only)
      if ((metrics[i]->isChannel0Set()) && (metrics[i]->isChannel1Set())) {
        try {
          for (auto& e : tiles) {
            configChannel0[e] = metrics[i]->getChannel0();
            configChannel1[e] = metrics[i]->getChannel1();
          }
        }
        catch (...) {
          std::stringstream msg;
          msg << "Channel specifications in aie_profile.graphs." << modName << " are not valid and hence ignored.";
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        }
      }
    } // Graph Pass 1b
  }

  void AieProfileMetadata::populateTilesConfigMetricsForTilesUsingJson(const int moduleIdx, 
        const module_type mod, MetricsCollectionManager& metricsCollectionManager)
  {
    std::string metricSettingsName = moduleNames[moduleIdx];
    uint8_t rowOffset     = (mod == module_type::mem_tile) ? 1 : metadataReader->getAIETileRowOffset();
    std::string entryName = (mod == module_type::mem_tile) ? "buffer" : "kernel";
    std::string modName   = (mod == module_type::core) ? "aie" 
                          : ((mod == module_type::dma) ? "aie_memory" : "memory_tile");

    std::set<tile_type> allValidTiles;
    auto validTilesVec = metadataReader->getTiles("all", mod, "all");
    std::unique_copy(validTilesVec.begin(), validTilesVec.end(), std::inserter(allValidTiles, allValidTiles.end()),
                     xdp::aie::tileCompare);

 

      const MetricCollection& tilesMetricCollection = metricsCollectionManager.getMetricCollection(mod, metricSettingsName);
      const auto& metrics = tilesMetricCollection.metrics;

    // NOTE: Only one of the following setting type can be specified in the JSON for a tile type.
    // Step 1a: Process all tiles metric setting ( "all_tiles"/"all_graphs" )
    // Step 1b: Process only range of tiles metric setting
    // Step 1c: Process single tile metric setting

    bool isAllTilesSet = false;
    bool isTileRangeSet = false;

    // Step 1a: Process "all_tiles" tiles metric setting
    for (size_t i = 0; i < metrics.size(); ++i) {

      if (!metrics[i]->isTileBased()) {
        xrt_core::message::send(severity_level::warning, "XRT",
                                "JSON Settings: Skipping metric " + metrics[i]->getMetric() + 
                                " as it is not tile-based for aie module.");
        continue;
      }

      // Check if all tiles are set or is already processed
      if ((!metrics[i]->isAllTilesSet()) || isAllTilesSet)
        break;

      auto tiles = metadataReader->getTiles("all", mod, "all");
      for (auto& e : tiles)
        configMetrics[moduleIdx][e] = metrics[i]->getMetric();

      // Use channel numbers if specified
      if (metrics[i]->isChannel0Set()) {
        for (auto& e : tiles)
          configChannel0[e] = metrics[i]->getChannel0();
      }
      if (metrics[i]->isChannel1Set()) {
        for (auto& e : tiles)
          configChannel1[e] = metrics[i]->getChannel1();
      }
      isAllTilesSet = true;
    } // Pass 1a

    // Step 1b: Process tiles range metric settings
    for (size_t i = 0; i < metrics.size(); ++i) {
      // NOTE: If all tiles/graphs are already set, skip this pass
      if (isAllTilesSet)
        break;

      if (!metrics[i]->isTileBased()) {
        xrt_core::message::send(severity_level::warning, "XRT",
                                "JSON Settings: Skipping metric " + metrics[i]->getMetric() + 
                                " as it is not tile-based for " + modName + " module.");
        continue;
      }

      if (!metrics[i]->isTilesRangeSet())
        break;
 
      uint8_t minRow = 0, minCol = 0;
      uint8_t maxRow = 0, maxCol = 0;

      try {
        std::vector<uint8_t> minTile;
        minTile = metrics[i]->getStartTile();

        std::vector<uint8_t> maxTile;
        maxTile = metrics[i]->getEndTile();
        if (maxTile.empty())
          maxTile = minTile;
        
        if (minTile.empty() || maxTile.empty()) {
          std::stringstream msg;
          msg << "Tile range specification in aie_profile.tiles." << modName
              << " is not a valid format and hence skipped. Should use \"start\": [column, row], \"end\": [column, row].";
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());
          continue;
        }
              
        minCol = minTile[0];
        minRow = minTile[1] + rowOffset;

        maxCol = maxTile[0];
        maxRow = maxTile[1] + rowOffset;
      }
      catch (...) {
        xrt_core::message::send(severity_level::warning, "XRT",
                                "Tile range specification in aie_profile.tiles." + modName
                                + " is not valid format and hence skipped.");
        continue;
      }

      // Ensure range is valid
      if ((minCol > maxCol) || (minRow > maxRow)) {
        std::stringstream msg;
        msg << "Tile range specification in aie_profile.tiles." << modName
            << " is not a valid range (start <= end) and hence skipped.";
        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        continue;
      }

      uint8_t channel0 = 0;
      uint8_t channel1 = 1;

      if (metrics[i]->areChannelsSet()) {
        try {
          channel0 = metrics[i]->getChannel0();
          channel1 = metrics[i]->getChannel1();
        }
        catch (...) {
          std::stringstream msg;
          msg << "Channel specifications in aie_profile.tiles." << modName 
              << " are not valid and hence ignored.";
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        }
      }

      for (uint8_t col = minCol; col <= maxCol; ++col) {
        for (uint8_t row = minRow; row <= maxRow; ++row) {
          tile_type tile;
          tile.col = col;
          tile.row = row;
          tile.active_core   = true;
          tile.active_memory = true;

          // Make sure tile is used
          auto it = std::find_if(allValidTiles.begin(), allValidTiles.end(),
            compareTileByLoc(tile));
          if (it == allValidTiles.end()) {
            std::stringstream msg;
            msg << "Specified Tile (" << std::to_string(tile.col) << "," 
                << std::to_string(tile.row) << ") is not active. Hence skipped.";
            xrt_core::message::send(severity_level::warning, "XRT", msg.str());
            continue;
          }

          configMetrics[moduleIdx][tile] = metrics[i]->getMetric();

          // Grab channel numbers (if specified; memory tiles only)
          if (metrics[i]->areChannelsSet()) {
            configChannel0[tile] = channel0;
            configChannel1[tile] = channel1;
          }

          isTileRangeSet = true;
        }
      }
    } // End of pass 1b

    // Step 1c: Process single tile metric settings
    for (size_t i = 0; i < metrics.size(); ++i) {
      if (isAllTilesSet || isTileRangeSet)
        break;

      if (!metrics[i]->isTileBased()) {
        xrt_core::message::send(severity_level::warning, "XRT",
                                "JSON Settings: Skipping metric " + metrics[i]->getMetric() + 
                                " as it is not tile-based for " + modName + " module.");
        continue;
      }

      try {
        uint8_t col, row;
        col = metrics[i]->getCol();
        row = metrics[i]->getRow() + rowOffset;

        tile_type tile;
        tile.col = col;
        tile.row = row;
        tile.active_core   = true;
        tile.active_memory = true;

        // Make sure tile is used
        auto it = std::find_if(allValidTiles.begin(), allValidTiles.end(),
                               compareTileByLoc(tile));
        if (it == allValidTiles.end()) {
          std::stringstream msg;
          msg << "Specified Tile (" << std::to_string(tile.col) << "," 
              << std::to_string(tile.row) << ") is not active. Hence skipped.";
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());
          continue;
        }

        configMetrics[moduleIdx][tile] = metrics[i]->getMetric();

        // Grab channel numbers (if specified; memory tiles only)
        if (metrics[i]->areChannelsSet()) {
          configChannel0[tile] = metrics[i]->getChannel0();
          configChannel1[tile] = metrics[i]->getChannel1();
        }
      }
      catch (...) {
        xrt_core::message::send(severity_level::warning, "XRT",
                                "Tile range specification in aie_profile.tiles." + modName
                                + " is not valid format and hence skipped.");
        continue;
      }
    } // End of pass 1c
} // end of populateTilesConfigMetricsForTilesUsingJson

  void AieProfileMetadata::getConfigMetricsForTilesUsingJson(const int moduleIdx, 
      const module_type mod, MetricsCollectionManager& metricsCollectionManager)
  {
    std::string metricSettingsName = moduleNames[moduleIdx];

    // const std::vector<std::unique_ptr<Metric>>* metrics = nullptr;
    try {
      const MetricCollection& tilesMetricCollection = metricsCollectionManager.getMetricCollection(mod, metricSettingsName);
      const auto& metrics = tilesMetricCollection.metrics;

    if (metrics.empty()) {
      xrt_core::message::send(severity_level::debug, "XRT",
                              "No metric settings found for " + metricSettingsName);
      return;
    }

    if ((metadataReader->getHardwareGeneration() == 1) && (mod == module_type::mem_tile)) {
      xrt_core::message::send(severity_level::warning, "XRT",
                              "Memory tiles are not available in AIE1. Profile "
                              "settings will be ignored.");
      return;
    }

    if (tilesMetricCollection.isGraphBased())
      populateGraphConfigMetricsForTilesUsingJson(moduleIdx, mod, metricsCollectionManager);
    else if (tilesMetricCollection.isTileBased())
      populateTilesConfigMetricsForTilesUsingJson(moduleIdx, mod, metricsCollectionManager);

    // Set default, check validity, and remove "off" tiles
    auto defaultSet = defaultSets[moduleIdx];
    bool showWarning = true;
    std::vector<tile_type> offTiles;

    for (auto& tileMetric : configMetrics[moduleIdx]) {
      auto tile = tileMetric.first;
      auto metricSet = tileMetric.second;

      // Save list of "off" tiles
      if (metricSet.empty() || (metricSet.compare("off") == 0)) {
        offTiles.push_back(tile);
        continue;
      }

      // Ensure requested metric set is supported (if not, use default)
      if (std::find(metricStrings.at(mod).begin(), metricStrings.at(mod).end(), metricSet) ==
          metricStrings.at(mod).end()) {
        if (showWarning) {
          std::stringstream msg;
          msg << "Unable to find " << moduleNames[moduleIdx] << " metric set " << metricSet
              << ". Using default of " << defaultSet << ".";
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());
          showWarning = false;
        }

        tileMetric.second = defaultSet;
        metricSet = defaultSet;
      }

      // Specify complementary metric sets (as needed)
      // NOTE 1: Issue warning when we replace their setting
      // NOTE 2: This is agnostic to order and which setting is specified
      auto pairModuleIdx = getPairModuleIndex(metricSet, mod);
      if (pairModuleIdx >= 0) {
        auto pairItr = std::find_if(configMetrics[pairModuleIdx].begin(), 
          configMetrics[pairModuleIdx].end(), compareTileByLocMap(tile));

        if ((pairItr != configMetrics[pairModuleIdx].end())
            && (pairItr->second != metricSet)) {
          std::stringstream msg;
          msg << "Replacing metric set " << pairItr->second << " with complementary set " 
              << metricSet << " for tile (" << std::to_string(tile.col) << ","
              << std::to_string(tile.row) << ") [1].";
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        }

        configMetrics[pairModuleIdx][tile] = metricSet;
        // Protect this setting by adding it to secondary map
        pairConfigMetrics[tile] = metricSet;
      }
      else {
        // Check if this tile/module was previously protected
        auto pairItr2 = std::find_if(pairConfigMetrics.begin(), 
          pairConfigMetrics.end(), compareTileByLocMap(tile));

        if (pairItr2 != pairConfigMetrics.end()
            && (pairItr2->second != metricSet)) {
          std::stringstream msg;
          msg << "Replacing metric set " << metricSet << " with complementary set " 
              << pairItr2->second << " for tile (" << std::to_string(tile.col) << ","
              << std::to_string(tile.row) << ") [2].";
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());
          configMetrics[moduleIdx][tile] = pairItr2->second;
        }
      }
    }

      // Remove all the "off" tiles
      for (auto& t : offTiles) {
        configMetrics[moduleIdx].erase(t);
      }
    } catch (const std::exception& e) {
      xrt_core::message::send(severity_level::error, "XRT", e.what());
      return;
    }
  } // end of getConfigMetricsForTilesUsingJson

  void AieProfileMetadata::getConfigMetricsForInterfaceTilesUsingJson(const int moduleIdx,
      MetricsCollectionManager& metricsCollectionManager)
    {
      std::string metricSettingsName = moduleNames[moduleIdx];

      try {
        const MetricCollection& tilesMetricCollection = metricsCollectionManager.getMetricCollection(module_type::shim, metricSettingsName);
        const auto& metrics = tilesMetricCollection.metrics;
        if (metrics.empty()) {
          xrt_core::message::send(severity_level::debug, "XRT",
                                  "No metric settings found for " + metricSettingsName);
          return;
        }

        if (tilesMetricCollection.isGraphBased())
          populateGraphConfigMetricsForInterfaceTilesUsingJson(moduleIdx, module_type::shim, metricsCollectionManager);
        else if (tilesMetricCollection.isTileBased())
          populateTilesConfigMetricsForInterfaceTilesUsingJson(moduleIdx, module_type::shim, metricsCollectionManager);


        // auto allValidGraphs = metadataReader->getValidGraphs();
        // auto allValidPorts  = metadataReader->getValidPorts();
      
          // Set default, check validity, and remove "off" tiles
          auto defaultSet = defaultSets[moduleIdx];
          bool showWarning = true;
          std::vector<tile_type> offTiles;
          auto metricVec = metricStrings.at(module_type::shim);

          for (auto& tileMetric : configMetrics[moduleIdx]) {
            // Save list of "off" tiles
            if (tileMetric.second.empty() || (tileMetric.second.compare("off") == 0)) {
              offTiles.push_back(tileMetric.first);
              continue;
            }

            // Ensure requested metric set is supported (if not, use default)
            if (std::find(metricVec.begin(), metricVec.end(), tileMetric.second) == metricVec.end()) {
              if (showWarning) {
                std::string msg = "Unable to find interface_tile metric set " + tileMetric.second
                                  + ". Using default of " + defaultSet + ". ";
                xrt_core::message::send(severity_level::warning, "XRT", msg);
                showWarning = false;
              }

              tileMetric.second = defaultSet;
            }
          }

          // Remove all the "off" tiles
          for (auto& t : offTiles) {
            configMetrics[moduleIdx].erase(t);
          }

          // Print configMetrics for moduleIdx.
          for (auto & tileMetric : configMetrics[moduleIdx]) {
            auto tile = tileMetric.first;
            auto metricSet = tileMetric.second;
          }
        } catch (const std::exception& e) {
          xrt_core::message::send(severity_level::error, "XRT", e.what());
          return;
        }
    } // end of getConfigMetricsForInterfaceTilesUsingJson


   void AieProfileMetadata::populateGraphConfigMetricsForInterfaceTilesUsingJson(const int moduleIdx,
      const module_type mod, MetricsCollectionManager& metricsCollectionManager)
  {
    std::string metricSettingsName = moduleNames[moduleIdx];
    const MetricCollection& tilesMetricCollection = metricsCollectionManager.getMetricCollection(mod, metricSettingsName);
    const auto& metrics = tilesMetricCollection.metrics;

    auto allValidGraphs = metadataReader->getValidGraphs();
    auto allValidPorts  = metadataReader->getValidPorts();
 
    // Only one graphs setting type is supported at a time in JSON.
    // Step 1a: Process all graphs metric setting ("graph": "all")
    // Step 1b: Process single graph metric setting ("graph": "<graph name>")

    bool allGraphs = false;

    // Step 1a: Process all graphs metric setting ( "all_graphs" ) 
    for (size_t i = 0; i < metrics.size(); ++i) {
      if (!metrics[i]->isGraphBased()) {
        xrt_core::message::send(severity_level::warning, "XRT",
                                "JSON Settings: Skipping metric " + metrics[i]->getMetric() + 
                                " as it is not graph-based for interface_tile module.");
        continue;
      }

      // Check if graph is not all or if invalid kernel
      if (!metrics[i]->isAllTilesSet())
        continue;
      
      // Check if all graphs setting is already processed
      if (allGraphs)
        break;

      std::string graphName = metrics[i]->getGraph();
      std::string graphEntity = metrics[i]->getGraphEntity();

      if ((graphEntity != "all")
          && (std::find(allValidPorts.begin(), allValidPorts.end(), graphEntity) == allValidPorts.end())) {
        std::stringstream msg;
        msg << "Could not find port " << graphEntity
            << " as specified in aie_profile.graphs.interface_tile setting."
            << " The following ports are valid : " << allValidPorts[0];

        for (size_t j = 1; j < allValidPorts.size(); j++)
          msg << ", " << allValidPorts[j];

        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        continue;
      }

      auto tiles = metadataReader->getInterfaceTiles(graphName,
                                          graphEntity,
                                          metrics[i]->getMetric());

      for (auto& e : tiles) {
        configMetrics[moduleIdx][e] = metrics[i]->getMetric();
      }

      // Grab channel numbers (if specified; memory tiles only)
        if (metrics[i]->getMetric() == METRIC_BYTE_COUNT) {
          uint32_t bytes = processUserSpecifiedBytes(metrics[i]->getBytesToTransfer());
          for (auto& e : tiles)
            setUserSpecifiedBytes(e, bytes);
        }
        else {
          try {
            if (metrics[i]->isChannel0Set()) {
              for (auto& e : tiles) {
                configChannel0[e] = metrics[i]->getChannel0();
                configChannel1[e] = metrics[i]->isChannel1Set() ? metrics[i]->getChannel1() : configChannel0[e];
              }
            }
          }
          catch (...) {
            std::stringstream msg;
            msg << "Channel specifications in aie_profile.graphs.interface_tile "
                << "are not valid and hence ignored.";
            xrt_core::message::send(severity_level::warning, "XRT", msg.str());
          }
        }
      allGraphs = true;
    } // Graph Pass 1a

    // Step 1b: Process single graph metric setting
    for (size_t i = 0; i < metrics.size(); ++i) {
 
      // Check if already processed or if invalid
      if (allGraphs)
        break;

      if (!metrics[i]->isGraphBased()) {
        xrt_core::message::send(severity_level::warning, "XRT",
                                "JSON Settings: Skipping metric " + metrics[i]->getMetric() + 
                                " as it is not graph-based for interface_tile module.");
        continue;
      }

      const std::string& graphName = metrics[i]->getGraph();
      const std::string& graphEntity = metrics[i]->getGraphEntity();
      if (std::find(allValidGraphs.begin(), allValidGraphs.end(), graphName) == allValidGraphs.end()) {
        std::stringstream msg;
        msg << "Could not find graph " << graphName
            << ", as specified in aie_profile.graphs.interface_tile setting."
            << " The following graphs are valid : " << allValidGraphs[0];

        for (size_t j = 1; j < allValidGraphs.size(); j++)
          msg << ", " << allValidGraphs[j];

        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        continue;
      }

      if ((graphEntity != "all")
          && (std::find(allValidPorts.begin(), allValidPorts.end(), graphEntity) == allValidPorts.end())) {
        std::stringstream msg;
        msg << "Could not find port " << graphEntity
            << ", as specified in aie_profile.graphs.interface_tile setting."
            << " The following ports are valid : " << allValidPorts[0];

        for (size_t j = 1; j < allValidPorts.size(); j++)
          msg << ", " << allValidPorts[j];

        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        continue;
      }

      auto tiles = metadataReader->getInterfaceTiles(graphName,
                                          graphEntity,
                                          metrics[i]->getMetric());

      for (auto& e : tiles) {
        configMetrics[moduleIdx][e] = metrics[i]->getMetric();
      }

      // Grab channel numbers (if specified; memory tiles only)
        if (metrics[i]->getMetric() == METRIC_BYTE_COUNT) {
          uint32_t bytes = processUserSpecifiedBytes(metrics[i]->getBytesToTransfer());
          for (auto& e : tiles)
            setUserSpecifiedBytes(e, bytes);
        }
        else {
          try {
            if (metrics[i]->isChannel0Set()) {
              for (auto& e : tiles) {
                configChannel0[e] = metrics[i]->getChannel0();
                configChannel1[e] = metrics[i]->isChannel1Set() ? metrics[i]->getChannel1() : configChannel0[e];
              }
            }
          }
          catch (...) {
            std::stringstream msg;
            msg << "Channel specifications in aie_profile.graphs.interface_tile "
                << "are not valid and hence ignored.";
            xrt_core::message::send(severity_level::warning, "XRT", msg.str());
          }
        }
    } // Graph Pass 2
  }

  void AieProfileMetadata::populateTilesConfigMetricsForInterfaceTilesUsingJson(const int moduleIdx,
      const module_type mod, MetricsCollectionManager& metricsCollectionManager)
  {
        if (mod != module_type::shim)
          return;

        std::string metricSettingsName = moduleNames[moduleIdx];
        const MetricCollection& tilesMetricCollection = metricsCollectionManager.getMetricCollection(module_type::shim, metricSettingsName);
        const auto& metrics = tilesMetricCollection.metrics;

        // Step 1a: Process "all" tiles metric setting
        // Step 1b: Process only range of tiles metric setting
        // Step 1c: Process single tile metric setting
        // NOTE: Only one of these can be specified in the JSON file for a tile type.
    
        bool isAllTilesSet = false;
        bool isTileRangeSet = false;

        // Pass 1a : process only "all" metric setting
        for (size_t i = 0; i < metrics.size(); ++i) {
          if (!metrics[i]->isAllTilesSet())
            break;
        
          // By-default select both the channels
          uint8_t channelId0 = 0;
          uint8_t channelId1 = 1;

          std::vector<tile_type> tiles;
          if (metrics[i]->isChannel0Set()) {
            channelId0 = metrics[i]->getChannel0();
            channelId1 = metrics[i]->isChannel1Set() ? metrics[i]->getChannel1() : channelId0;
            tiles = metadataReader->getInterfaceTiles("all", "all", metrics[i]->getMetric(), channelId0);
          }
          else
            tiles = metadataReader->getInterfaceTiles("all", "all", metrics[i]->getMetric());

          for (auto& t : tiles) {
            configMetrics[moduleIdx][t] = metrics[i]->getMetric();
            configChannel0[t] = channelId0;
            configChannel1[t] = channelId1;
          }
          isAllTilesSet = true;
        } // End of pass 1a

        // Step 1b: process tile range metric setting
        for (size_t i = 0; i < metrics.size(); ++i) {
          if (isAllTilesSet)
            break;
          
          if (!metrics[i]->isTilesRangeSet())
            break;
 
          // TODO: Add Support for Profile API metric sets
          // if (!isSupported(metrics[i]->getMetric(), true))
          //   continue;

          uint8_t minCol = 0, maxCol = 0;
          uint8_t minRow = 0, maxRow = 0;
          try {
            std::vector<uint8_t> minTile;
            minTile = metrics[i]->getStartTile();

            std::vector<uint8_t> maxTile;
            maxTile = metrics[i]->getEndTile();
            if (maxTile.empty())
              maxTile = minTile;
          
            if (minTile.empty() || maxTile.empty()) {
              std::stringstream msg;
              msg << "Tile range specification in aie_profile.tiles." << moduleNames[moduleIdx]
                  << " is not a valid format and hence skipped."
                  << "It should be \"start\": [mincolumn,minrow], \"end\" :[maxcolumn, maxrow]";
              xrt_core::message::send(severity_level::warning, "XRT", msg.str());
              continue;
            }
                
            minCol = minTile[0];
            maxCol = maxTile[0];
            minRow = minTile[1];
            maxRow = maxTile[1];
          }
          catch (...) {
            xrt_core::message::send(severity_level::warning, "XRT",
                                    "Tile range specification in aie_profile.tiles." + moduleNames[moduleIdx]
                                    + " is not valid format and hence skipped.");
            continue;
          }

          // Ensure range is valid
          if ((minCol > maxCol) || (minRow > maxRow)) {
            std::stringstream msg;
            msg << "Tile range specification in aie_profile.tiles." << moduleNames[moduleIdx]
                << " is not a valid range (start <= end) and hence skipped.";
            xrt_core::message::send(severity_level::warning, "XRT", msg.str());
            continue;
          }
          // By-default select both the channels
          bool foundChannels = false;
          uint8_t channelId0 = 0;
          uint8_t channelId1 = 1;
          if (metrics[i]->isChannel0Set()) {
            channelId0 = metrics[i]->getChannel0();
            channelId1 = metrics[i]->isChannel1Set() ? metrics[i]->getChannel1() : channelId0;
            foundChannels = true;
          }

          // uint32_t bytes = defaultTransferBytes;
          //TODO: Support for user specified bytes.
            // if (profileAPIMetricSet(metrics[i][1])) {
            //   bytes = processUserSpecifiedBytes(metrics[i][2]);
            // }

          int16_t channelNum = (foundChannels) ? channelId0 : -1;
          auto tiles = metadataReader->getInterfaceTiles("all", "all", metrics[i]->getMetric(), channelNum, true, minCol, maxCol);
          
          for (auto& t : tiles) {
            configMetrics[moduleIdx][t] = metrics[i]->getMetric();
            configChannel0[t] = channelId0;
            configChannel1[t] = channelId1;
          }
          isTileRangeSet = true;
        } // End of pass 1b

        // Pass 1c: process only single tile metric setting
        for (size_t i = 0; i < metrics.size(); ++i) {
          if(isAllTilesSet || isTileRangeSet)
            break;

          // TODO: Add Support for Profile API metric sets
          // if (!isSupported(metrics[i][1], true))
          //   continue;

          uint8_t col = 0;
          col = metrics[i]->getCol();
            
            // By-default select both the channels
            bool foundChannels = false;
            uint8_t channelId0 = 0;
            uint8_t channelId1 = 1;

            if (metrics[i]->isChannel0Set()) {
              foundChannels = true;
              channelId0 = metrics[i]->getChannel0();
              channelId1 = metrics[i]->isChannel1Set() ? metrics[i]->getChannel1() : channelId0;
            }
                
            int16_t channelNum = (foundChannels) ? channelId0 : -1;
            auto tiles = metadataReader->getInterfaceTiles("all", "all", metrics[i]->getMetric(), channelNum, true, col, col);
            
            for (auto& t : tiles) {
              configMetrics[moduleIdx][t] = metrics[i]->getMetric();
              configChannel0[t] = channelId0;
              configChannel1[t] = channelId1;
              // if (metrics[i]->getMetric() == METRIC_BYTE_COUNT)
              //   setUserSpecifiedBytes(t, bytes);
            }
          } // Pass 1c: process single tile metric setting


  }

  /****************************************************************************
   * Resolve metrics for micrcontrollers
   ***************************************************************************/
  void AieProfileMetadata::getConfigMetricsForMicrocontrollersUsingJson(const int moduleIdx,
      MetricsCollectionManager& metricsCollectionManager)
  {
    std::string metricSettingsName = moduleNames[moduleIdx];
    try {
      const MetricCollection& tilesMetricCollection = metricsCollectionManager.getMetricCollection(module_type::uc, metricSettingsName);
      const auto& metrics = tilesMetricCollection.metrics;
      if (metrics.empty()) {
        xrt_core::message::send(severity_level::debug, "XRT",
                                "No metric settings found for " + metricSettingsName);
        return;
      }

      auto allValidGraphs = metadataReader->getValidGraphs();
      auto allValidPorts = metadataReader->getValidPorts();

      // Step 1a: Process "all" tiles metric setting
      // Step 1b: Process only range of tiles metric setting
      // Step 1c: Process single tile metric setting
      // NOTE: Only one of these can be specified in the JSON file for a tile type.
      bool isAllTilesSet = false;
      bool isTileRangeSet = false;

      // Step 1a: Process "all" tiles metric setting
      for (size_t i = 0; i < metrics.size(); ++i) {
        if (metrics[i]->isAllTilesSet() == false)
          break;

        auto tiles = metadataReader->getMicrocontrollers(false);

        for (auto& t : tiles)
          configMetrics[moduleIdx][t] = metrics[i]->getMetric();
        
        isAllTilesSet = true;
      } // end of pass 1a

      // Step 1b: Process only range of tiles metric setting
      for (size_t i = 0; i < metrics.size(); ++i) {
        if (isAllTilesSet)
          break;

        if (!metrics[i]->isTilesRangeSet())
          break;
 
        uint8_t minCol = 0;
        minCol = metrics[i]->getStartTile().front();

        uint8_t maxCol = 0;
        maxCol = metrics[i]->getEndTile().front();

        auto tiles = metadataReader->getMicrocontrollers(true, minCol, maxCol);
        
        for (auto& t : tiles)
          configMetrics[moduleIdx][t] = metrics[i]->getMetric();
        
        isTileRangeSet = true;
      } // end of pass 1b

      // Process only single tile metric setting
      for (size_t i = 0; i < metrics.size(); ++i) {
        if (isAllTilesSet || isTileRangeSet)
          break;

        uint8_t col = 0;
        try {
          col = metrics[i]->getStartTile().front();
        }
        catch (std::invalid_argument const&) {
            // Expected column specification is not a number. Give warning and skip
            xrt_core::message::send(severity_level::warning, "XRT",
                                    "Column specification in aie_profile.tiles.microcontroller "
                                    "is not an integer and hence skipped.");
            continue;
        }
        auto tiles = metadataReader->getMicrocontrollers(true, col, col);
          
        for (auto& t : tiles)
          configMetrics[moduleIdx][t] = metrics[i]->getMetric();
      }

      // Set default, check validity, and remove "off" tiles
      auto defaultSet = defaultSets[moduleIdx];
      bool showWarning = true;
      std::vector<tile_type> offTiles;
      auto metricVec = metricStrings.at(module_type::uc);

      for (auto& tileMetric : configMetrics[moduleIdx]) {
        // Save list of "off" tiles
        if (tileMetric.second.empty() || (tileMetric.second.compare("off") == 0)) {
          offTiles.push_back(tileMetric.first);
          continue;
        }

        // Ensure requested metric set is supported (if not, use default)
        if (std::find(metricVec.begin(), metricVec.end(), tileMetric.second) == metricVec.end()) {
          if (showWarning) {
            std::string msg = "Unable to find microcontroller metric set " + tileMetric.second
                              + ". Using default of " + defaultSet + ". ";
            xrt_core::message::send(severity_level::warning, "XRT", msg);
            showWarning = false;
          }

          tileMetric.second = defaultSet;
        }
      }

      // Remove all the "off" tiles
      for (auto& t : offTiles)
        configMetrics[moduleIdx].erase(t);
    } catch (const std::exception& e) {
      xrt_core::message::send(severity_level::error, "XRT", e.what());
      return;
    }
  } // end of getConfigMetricsForMicrocontrollersUsingJson
}  // namespace xdp
