// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved
#ifndef METRICS_H
#define METRICS_H

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <string>
#include <memory>
#include <optional>
#include <vector>
#include <fstream>
#include <sstream>
#include "core/common/message.h"
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/database/parser/parser_utils.h"
#include "xdp/profile/database/parser/metrics_type.h"

namespace xdp {
  namespace pt = boost::property_tree;
  using severity_level = xrt_core::message::severity_level;

  // --------------------------------------------------------------------------------------------------------------------
  // Base interface for all metrics
  class Metric {
  public:
      Metric(std::string metric, std::optional<std::vector<uint8_t>> channels, 
                   std::optional<std::string> bytes)
        : metric(std::move(metric)), channels(std::move(channels)), bytes_to_transfer(std::move(bytes)) {}
      virtual ~Metric() = default;

      virtual void setAllTiles(bool allTiles) { allTilesRange = allTiles; }
      virtual bool isAllTilesSet() const      { return allTilesRange; }
      virtual void setTilesRange(bool range)  { tileRange = range; }
      virtual bool isTilesRangeSet() const    { return tileRange; }

      virtual std::string getGraph() const { return ""; }
      virtual std::string getGraphEntity() const { return ""; }

      virtual std::vector<uint8_t> getStartTile() const { return {}; }
      virtual std::vector<uint8_t> getEndTile() const { return {}; }
      virtual uint8_t getCol() const { return 0; }
      virtual uint8_t getRow() const { return 0; }

      virtual bool isGraphBased() const { return false; }
      virtual bool isTileBased() const { return false; }
 
      virtual void print() const;
      // Convert metric to ptree
      virtual boost::property_tree::ptree toPtree() const = 0;
  public:
    std::string metric;
    std::optional<std::vector<uint8_t>> channels;
    std::optional<std::string> bytes_to_transfer;
    bool allTilesRange = false;
    bool tileRange = false;

    const std::string& getMetric() const { return metric; }
    bool areChannelsSet() const;
    bool isChannel0Set() const;
    bool isChannel1Set() const;
    uint8_t getChannel0() const;
    uint8_t getChannel1() const;
    std::optional<uint8_t> getChannel0Safe() const;
    std::optional<uint8_t> getChannel1Safe() const;
    std::string getBytesToTransfer() const;

    // Metric(std::string metric, std::optional<std::vector<uint8_t>> ch = std::nullopt, 
    //        std::optional<std::string> bytes = std::nullopt);
    // Add common fields to ptree
    void addCommonFields(boost::property_tree::ptree& obj) const;
  };

  // TileBasedMetricEntry class
  class TileBasedMetricEntry : public Metric {
  public:
      std::vector<uint8_t> startTile;
      std::vector<uint8_t> endTile;
      uint8_t col, row;

    // Constructor based on column and row per tile
    TileBasedMetricEntry(uint8_t c, uint8_t r, std::string metric, 
                         std::optional<std::vector<uint8_t>> channels, std::optional<std::string> bytes)
        : Metric(std::move(metric), std::move(channels), std::move(bytes)), col(c), row(r) {}

    // Constructor based on start and end tiles range or "all" tiles
    TileBasedMetricEntry(std::vector<uint8_t> startTile, std::vector<uint8_t> endTile, std::string metric, 
                         std::optional<std::vector<uint8_t>> channels, std::optional<std::string> bytes)
        : Metric(std::move(metric), std::move(channels), std::move(bytes)), startTile(std::move(startTile)), endTile(std::move(endTile)) {}
     
      // Create from ptree
      static std::unique_ptr<Metric> processSettings(const MetricType& type, const boost::property_tree::ptree& obj);
      
      virtual std::vector<uint8_t> getStartTile() const override { return startTile; }
      virtual std::vector<uint8_t> getEndTile() const override   { return endTile; }
      virtual uint8_t getCol() const override { return col; }
      virtual uint8_t getRow() const override { return row; }
      
      virtual bool isGraphBased() const override { return false; }
      virtual bool isTileBased() const override  { return true; }
 
      // Debug Methods
      boost::property_tree::ptree toPtree() const override;
      void print() const;
  };

  class GraphBasedMetricEntry : public Metric {
  public:
    std::string graph;
    
    GraphBasedMetricEntry(std::string graph, std::string metric, 
                         std::optional<std::vector<uint8_t>> channels = std::nullopt, 
                         std::optional<std::string> bytes = std::nullopt)
        : Metric(std::move(metric), std::move(channels), std::move(bytes))
        , graph(std::move(graph)) {}

    static std::unique_ptr<Metric> processSettings(const MetricType& type, const boost::property_tree::ptree& obj);
 
    // Override base class methods
    std::string getGraph() const override { return graph; }
    bool isGraphBased() const override { return true; }
    
    // Virtual methods with default implementations (empty for base class)
    virtual std::string getKernel() const { return ""; }
    virtual std::string getBuffer() const { return ""; }
    virtual std::string getPort() const { return ""; }
    
    // For backward compatibility
    std::string getGraphEntity() const override {
        // Try each entity type and return the first non-empty one
        std::string entity = getKernel();
        if (!entity.empty()) return entity;
        
        entity = getBuffer();
        if (!entity.empty()) return entity;
        
        entity = getPort();
        if (!entity.empty()) return entity;
        
        return "all";  // fallback
    }
    
    // Base implementation for ptree and print
    virtual boost::property_tree::ptree toPtree() const = 0;
    virtual void print() const = 0;
};

// AIE-specific (kernel-based)
class AIEGraphBasedMetricEntry : public GraphBasedMetricEntry {
public:
    std::string kernel;
    
    AIEGraphBasedMetricEntry(std::string graph, std::string kernel, std::string metric,
                            std::optional<std::vector<uint8_t>> channels = std::nullopt,
                            std::optional<std::string> bytes = std::nullopt)
        : GraphBasedMetricEntry(std::move(graph), std::move(metric), std::move(channels), std::move(bytes))
        , kernel(std::move(kernel)) {}

    // Override base class method
    std::string getKernel() const override { return kernel; }

   
    boost::property_tree::ptree toPtree() const override {
        boost::property_tree::ptree obj;
        obj.put("graph", graph);
        obj.put("kernel", kernel);
        addCommonFields(obj);
        return obj;
    }

    void print() const override {
    }
};

// Memory tile-specific (buffer-based)
class MemoryTileGraphBasedMetricEntry : public GraphBasedMetricEntry {
public:
    std::string buffer;
    
    MemoryTileGraphBasedMetricEntry(std::string graph, std::string buffer, std::string metric,
                                   std::optional<std::vector<uint8_t>> channels = std::nullopt,
                                   std::optional<std::string> bytes = std::nullopt)
        : GraphBasedMetricEntry(std::move(graph), std::move(metric), std::move(channels), std::move(bytes))
        , buffer(std::move(buffer)) {}

    std::string getBuffer() const override { return buffer; }
    
    boost::property_tree::ptree toPtree() const override {
        boost::property_tree::ptree obj;
        obj.put("graph", graph);
        obj.put("buffer", buffer);
        addCommonFields(obj);
        return obj;
    }

    void print() const override {
    }
};

// Interface tile-specific (port-based)
class InterfaceTileGraphBasedMetricEntry : public GraphBasedMetricEntry {
public:
    std::string port;
    
    InterfaceTileGraphBasedMetricEntry(std::string graph, std::string port, std::string metric,
                                      std::optional<std::vector<uint8_t>> channels = std::nullopt,
                                      std::optional<std::string> bytes = std::nullopt)
        : GraphBasedMetricEntry(std::move(graph), std::move(metric), std::move(channels), std::move(bytes))
        , port(std::move(port)) {}

    std::string getPort() const override { return port; }
    
    boost::property_tree::ptree toPtree() const override {
        boost::property_tree::ptree obj;
        obj.put("graph", graph);
        obj.put("port", port);
        addCommonFields(obj);
        return obj;
    }

    void print() const override {
    }
};


}

#endif
