// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "aie_trace_metadata.h"

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

  void AieTraceMetadata::getConfigMetricsUsingJson(const int module, const module_type type,
                                  MetricsCollectionManager& metricsCollectionManager)
  {
    if (type == module_type::shim)
        getConfigMetricsForInterfaceTilesUsingJson(module, metricsCollectionManager);
    else
        getConfigMetricsForTilesUsingJson(module, type, metricsCollectionManager);
  }

  /****************************************************************************
   * Resolve metrics for AIE tiles (aie_tile module combines core and memory)
   ***************************************************************************/
  void AieTraceMetadata::populateGraphConfigMetricsForTilesUsingJson(const int /*moduleIdx*/, 
      const module_type mod, MetricsCollectionManager& metricsCollectionManager)
  {
    // Determine the correct metric settings name based on module type
    std::string metricSettingsName;
    std::string modName;
    if (mod == module_type::mem_tile) {
      metricSettingsName = "memory_tile";
      modName = "memory_tile";
    } else {
      metricSettingsName = "aie_tile"; // For core and dma types
      modName = "aie_tile";
    }
    std::string entryName = "kernel";

    auto allValidGraphs  = metadataReader->getValidGraphs();
    auto allValidKernels = metadataReader->getValidKernels();

    std::set<tile_type> allValidTiles;
    auto validTilesVec = metadataReader->getTiles("all", mod, "all");
    std::unique_copy(validTilesVec.begin(), validTilesVec.end(), std::inserter(allValidTiles, allValidTiles.end()),
                     xdp::aie::tileCompare);
    const MetricCollection& tilesMetricCollection = metricsCollectionManager.getMetricCollection(mod, metricSettingsName);
    const auto& metrics = tilesMetricCollection.metrics;

    // Parse per-graph or per-kernel settings

    /*
     * Example JSON config format for AIE Tiles (aie_trace):
     *
     * {
     *   "graphs": {
     *     "aie_tile": [
     *       {
     *         "graph": "<graph name|all>",
     *         "kernel": "<kernel name|all>",
     *         "metric": "<off|execution|floating_point|stalls|write_throughputs|read_throughputs>"
     *       }
     *     ]
     *   }
     * }
     */

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
          (std::find(allValidKernels.begin(), allValidKernels.end(), graphEntity) == allValidKernels.end())) {
        std::stringstream msg;
        msg << "Could not find " << entryName << " " << graphEntity
            << " as specified in aie_trace.graphs." << modName << " setting."
            << " The following " << entryName << "s are valid : " << allValidKernels[0];

        for (size_t j = 1; j < allValidKernels.size(); j++)
          msg << ", " << allValidKernels[j];

        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        continue;
      }

      // For aie_trace, aie_tile applies to DMA type (includes both core and DMA functionality)
      auto tiles = metadataReader->getTiles(graphName, module_type::dma, graphEntity);
      for (auto& e : tiles) {
        configMetrics[e] = metrics[i]->getMetric();
      }

      // Grab channel numbers (if specified)
      if ((metrics[i]->isChannel0Set()) && (metrics[i]->isChannel1Set())) {
        try {
          for (auto& e : tiles) {
            configChannel0[e] = metrics[i]->getChannel0();
            configChannel1[e] = metrics[i]->getChannel1();
          }
        }
        catch (...) {
          std::stringstream msg;
          msg << "Channel specifications in aie_trace.graphs." << modName << " are not valid and hence ignored.";
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
      if ((graphEntity != "all") &&
          (std::find(allValidKernels.begin(), allValidKernels.end(), graphEntity) == allValidKernels.end())) {
        std::stringstream msg;
        msg << "Could not find " << entryName << " " << graphEntity
            << " as specified in aie_trace.graphs." << modName << " setting."
            << " The following " << entryName << "s are valid : " << allValidKernels[0];

        for (size_t j = 1; j < allValidKernels.size(); j++)
          msg << ", " << allValidKernels[j];

        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        continue;
      }

      // For aie_trace, aie_tile applies to DMA type (includes both core and DMA functionality)
      auto tiles = metadataReader->getTiles(graphName, module_type::dma, graphEntity);
      for (auto& e : tiles) {
        configMetrics[e] = metrics[i]->getMetric();
      }

      // Grab channel numbers (if specified)
      if ((metrics[i]->isChannel0Set()) && (metrics[i]->isChannel1Set())) {
        try {
          for (auto& e : tiles) {
            configChannel0[e] = metrics[i]->getChannel0();
            configChannel1[e] = metrics[i]->getChannel1();
          }
        }
        catch (...) {
          std::stringstream msg;
          msg << "Channel specifications in aie_trace.graphs." << modName << " are not valid and hence ignored.";
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        }
      }
    } // Graph Pass 1b
  }

  void AieTraceMetadata::populateTilesConfigMetricsForTilesUsingJson(const int /*moduleIdx*/, 
        const module_type mod, MetricsCollectionManager& metricsCollectionManager)
  {
    // Determine the correct metric settings name based on module type
    std::string metricSettingsName;
    std::string modName;
    if (mod == module_type::mem_tile) {
      metricSettingsName = "memory_tile";
      modName = "memory_tile";
    } else {
      metricSettingsName = "aie_tile"; // For core and dma types
      modName = "aie_tile";
    }
    uint8_t rowOffset = (mod == module_type::mem_tile) ? 1 : metadataReader->getAIETileRowOffset();

    std::set<tile_type> allValidTiles;
    auto validTilesVec = metadataReader->getTiles("all", mod, "all");
    std::unique_copy(validTilesVec.begin(), validTilesVec.end(), std::inserter(allValidTiles, allValidTiles.end()),
                     xdp::aie::tileCompare);

    const MetricCollection& tilesMetricCollection = metricsCollectionManager.getMetricCollection(mod, metricSettingsName);
    const auto& metrics = tilesMetricCollection.metrics;

    bool isAllTilesSet = false;
    bool isTileRangeSet = false;

    // Step 1a: Process "all_tiles" tiles metric setting
    for (size_t i = 0; i < metrics.size(); ++i) {

      if (!metrics[i]->isTileBased()) {
        xrt_core::message::send(severity_level::warning, "XRT",
                                "JSON Settings: Skipping metric " + metrics[i]->getMetric() + 
                                " as it is not tile-based for " + modName + " module.");
        continue;
      }

      // Check if all tiles are set or is already processed
      if ((!metrics[i]->isAllTilesSet()) || isAllTilesSet)
        break;

      auto tiles = metadataReader->getTiles("all", mod, "all");
      for (auto& e : tiles)
        configMetrics[e] = metrics[i]->getMetric();

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
          msg << "Tile range specification in aie_trace.tiles." << modName
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
        std::stringstream msg;
        msg << "Valid Tile range specification in aie_trace.tiles." << modName
            << " is not met, it will be re-processed for single-tile specification.";
        xrt_core::message::send(severity_level::info, "XRT", msg.str());
        continue;
      }

      // Ensure range is valid
      if ((minCol > maxCol) || (minRow > maxRow)) {
        std::stringstream msg;
        msg << "Tile range specification in aie_trace.tiles." << modName
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
          msg << "Channel specifications in aie_trace.tiles." << modName 
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

          configMetrics[tile] = metrics[i]->getMetric();

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

        configMetrics[tile] = metrics[i]->getMetric();

        // Grab channel numbers (if specified; memory tiles only)
        if (metrics[i]->areChannelsSet()) {
          configChannel0[tile] = metrics[i]->getChannel0();
          configChannel1[tile] = metrics[i]->getChannel1();
        }
      }
      catch (...) {
        xrt_core::message::send(severity_level::warning, "XRT",
                                "Tile range specification in aie_trace.tiles." + modName
                                + " is not valid format and hence skipped.");
        continue;
      }
    } // End of pass 1c
} // end of populateTilesConfigMetricsForTilesUsingJson

  void AieTraceMetadata::getConfigMetricsForTilesUsingJson(const int moduleIdx, 
      const module_type mod, MetricsCollectionManager& metricsCollectionManager)
  {
    // Determine the correct metric settings name based on module type
    std::string metricSettingsName;
    if (mod == module_type::mem_tile) {
      metricSettingsName = "memory_tile";
    } else {
      metricSettingsName = "aie_tile"; // For core and dma types
    }

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
                                "Memory tiles are not available in AIE1. Trace "
                                "settings will be ignored.");
        return;
      }

      if (tilesMetricCollection.isGraphBased())
        populateGraphConfigMetricsForTilesUsingJson(moduleIdx, mod, metricsCollectionManager);
      else if (tilesMetricCollection.isTileBased())
        populateTilesConfigMetricsForTilesUsingJson(moduleIdx, mod, metricsCollectionManager);

      // Get all valid tiles for validation
      std::set<tile_type> allValidTiles;
      auto validTilesVec = metadataReader->getTiles("all", mod, "all");
      std::unique_copy(validTilesVec.begin(), validTilesVec.end(), std::inserter(allValidTiles, allValidTiles.end()),
                       xdp::aie::tileCompare);

      // Set default, check validity, and remove "off" tiles
      bool showWarning = true;
      std::vector<tile_type> offTiles;
      auto coreSets = metricSets[module_type::core];
      auto memSets = metricSets[module_type::mem_tile];

      for (auto& tileMetric : configMetrics) {
        // Ignore other types of tiles
        if (allValidTiles.find(tileMetric.first) == allValidTiles.end())
          continue;
        // Save list of "off" tiles
        if (tileMetric.second.empty() || (tileMetric.second.compare("off") == 0)) {
          offTiles.push_back(tileMetric.first);
          continue;
        }

        // Ensure requested metric set is supported (if not, use default)
        if (((mod != module_type::mem_tile) 
            && (std::find(coreSets.begin(), coreSets.end(), tileMetric.second) == coreSets.cend()))
            || ((mod == module_type::mem_tile) 
            && (std::find(memSets.begin(), memSets.end(), tileMetric.second) == memSets.cend()))) {
          if (showWarning) {
            std::stringstream msg;
            msg << "Unable to find AIE trace metric set " << tileMetric.second 
                << ". Using default of " << defaultSets[mod] << ".";
            xrt_core::message::send(severity_level::warning, "XRT", msg.str());
            showWarning = false;
          }
          tileMetric.second = defaultSets[mod];
        }
      }

      // Remove all the "off" tiles
      for (auto& t : offTiles) {
        configMetrics.erase(t);
      }
    } catch (const std::exception& e) {
      xrt_core::message::send(severity_level::error, "XRT", e.what());
      return;
    }
  } // end of getConfigMetricsForTilesUsingJson

  void AieTraceMetadata::getConfigMetricsForInterfaceTilesUsingJson(const int moduleIdx,
      MetricsCollectionManager& metricsCollectionManager)
    {
      std::string metricSettingsName = "interface_tile";

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

        // Set default, check validity, and remove "off" tiles
        auto defaultSet = defaultSets[module_type::shim];
        bool showWarning = true;
        bool showWarningGMIOMetric = true;
        std::vector<tile_type> offTiles;
        auto metricVec = metricSets[module_type::shim];

        for (auto& tileMetric : configMetrics) {
          // Only validate interface tiles (row 0)
          if (tileMetric.first.row != 0)
            continue;
            
          // Save list of "off" tiles
          if (tileMetric.second.empty() || (tileMetric.second.compare("off") == 0)) {
            offTiles.push_back(tileMetric.first);
            continue;
          }

          // Check for PLIO tiles and it's compatible metric settings
          if ((tileMetric.first.subtype == io_type::PLIO) && isGMIOMetric(tileMetric.second)) {
            if (showWarningGMIOMetric) {
              std::string msg = "Configured interface_tile metric set " + tileMetric.second 
                              + " is only applicable for GMIO type tiles.";
              xrt_core::message::send(severity_level::warning, "XRT", msg);
              showWarningGMIOMetric = false;
            }

            std::stringstream msg;
            msg << "Configured interface_tile metric set metric set " << tileMetric.second;
            msg << " skipped for tile (" << +tileMetric.first.col << ", " << +tileMetric.first.row << ").";
            xrt_core::message::send(severity_level::debug, "XRT", msg.str());
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
          configMetrics.erase(t);
        }

      } catch (const std::exception& e) {
        xrt_core::message::send(severity_level::error, "XRT", e.what());
        return;
      }
    } // end of getConfigMetricsForInterfaceTilesUsingJson

   void AieTraceMetadata::populateGraphConfigMetricsForInterfaceTilesUsingJson(const int /*moduleIdx*/,
      const module_type mod, MetricsCollectionManager& metricsCollectionManager)
  {
    std::string metricSettingsName = "interface_tile";
    const MetricCollection& tilesMetricCollection = metricsCollectionManager.getMetricCollection(mod, metricSettingsName);
    const auto& metrics = tilesMetricCollection.metrics;

    auto allValidGraphs = metadataReader->getValidGraphs();
    auto allValidPorts  = metadataReader->getValidPorts();
 
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
            << ", as specified in aie_trace.graphs.interface_tile setting."
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
        configMetrics[e] = metrics[i]->getMetric();
      }

      // Grab channel numbers (if specified)
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
        msg << "Channel specifications in aie_trace.graphs.interface_tile "
            << "are not valid and hence ignored.";
        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
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
      
      // Validate graph name
      if (std::find(allValidGraphs.begin(), allValidGraphs.end(), graphName) == allValidGraphs.end()) {
        std::stringstream msg;
        msg << "Could not find graph " << graphName
            << ", as specified in aie_trace.graphs.interface_tile setting."
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
            << ", as specified in aie_trace.graphs.interface_tile setting."
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
        configMetrics[e] = metrics[i]->getMetric();
      }

      // Grab channel numbers (if specified)
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
        msg << "Channel specifications in aie_trace.graphs.interface_tile "
            << "are not valid and hence ignored.";
        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
      }
    } // Graph Pass 2
  }

  void AieTraceMetadata::populateTilesConfigMetricsForInterfaceTilesUsingJson(const int /*moduleIdx*/,
      const module_type mod, MetricsCollectionManager& metricsCollectionManager)
  {
        if (mod != module_type::shim)
          return;

        std::string metricSettingsName = "interface_tile";
        const MetricCollection& tilesMetricCollection = metricsCollectionManager.getMetricCollection(module_type::shim, metricSettingsName);
        const auto& metrics = tilesMetricCollection.metrics;

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
            configMetrics[t] = metrics[i]->getMetric();
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
              msg << "Tile range specification in aie_trace.tiles.interface_tile"
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
                                    "Tile range specification in aie_trace.tiles.interface_tile"
                                    " is not valid format and hence skipped.");
            continue;
          }

          // Ensure range is valid
          if ((minCol > maxCol) || (minRow > maxRow)) {
            std::stringstream msg;
            msg << "Tile range specification in aie_trace.tiles.interface_tile"
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

          int16_t channelNum = (foundChannels) ? channelId0 : -1;
          auto tiles = metadataReader->getInterfaceTiles("all", "all", metrics[i]->getMetric(), channelNum, true, minCol, maxCol);
          
          for (auto& t : tiles) {
            configMetrics[t] = metrics[i]->getMetric();
            configChannel0[t] = channelId0;
            configChannel1[t] = channelId1;
          }
          isTileRangeSet = true;
        } // End of pass 1b

        // Pass 1c: process only single tile metric setting
        for (size_t i = 0; i < metrics.size(); ++i) {
          if(isAllTilesSet || isTileRangeSet)
            break;

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
            configMetrics[t] = metrics[i]->getMetric();
            configChannel0[t] = channelId0;
            configChannel1[t] = channelId1;
          }
        } // Pass 1c: process single tile metric setting
  }

}  // namespace xdp
