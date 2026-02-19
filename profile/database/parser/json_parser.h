// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved
#ifndef XDP_JSON_PARSER_H
#define XDP_JSON_PARSER_H

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <string>
#include <map>
#include <memory>
#include <optional>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include "core/common/message.h"
#include "metrics_collection.h"
#include "xdp/profile/plugin/vp_base/info.h"
#include "xdp/profile/database/static_info/aie_constructs.h"

namespace xdp {
  namespace pt = boost::property_tree;
  using severity_level = xrt_core::message::severity_level;

  // Datastructures for JSON validation
    struct ValidationResult {
      bool isValid = true;
      std::vector<std::string> errors;
      std::vector<std::string> warnings;
      
      void addError(const std::string& message) {
          errors.push_back(message);
          isValid = false;
      }
      
      void addWarning(const std::string& message) {
          warnings.push_back(message);
      }
  };

  struct SchemaField {
      std::string name; // Json field name
      bool required;    // Is this field required?
      std::string type; // Expected type of the field (e.g. "string", "int", "array", etc.)
      std::vector<std::string> allowedValues; // Optional: allowed string values
      
      SchemaField(const std::string& n, bool req, const std::string& t, 
                  const std::vector<std::string>& allowed = {})
          : name(n), required(req), type(t), allowedValues(allowed) {}
  };

  struct PluginSettings {
      std::optional<uint32_t> intervalUs;
      std::optional<std::string> startType;
      std::optional<uint32_t> startIteration;
      
      bool hasIntervalUs() const { return intervalUs.has_value(); }
      bool hasStartType() const { return startType.has_value(); }
      bool hasStartIteration() const { return startIteration.has_value(); }
  };

  struct PluginJsonSetting {
      uint64_t pluginType;
      // "tiles"/"graphs" , <aie/aie_memory/memory_tile/interface_tile>, <JSON objects>>
      std::map<std::string, std::map<std::string, std::vector<pt::ptree>>> sections;
      PluginSettings settings;
      bool isValid = false;
      std::string errorMessage;
  };

  struct XdpJsonSetting {
      std::string version;
      std::map<uint64_t, PluginJsonSetting> plugins;
      bool isValid = false;
      std::string errorMessage;
  };

  struct JsonParseResult {
    pt::ptree tree;
    bool success = false;
    std::string errorMessage;

    bool isValid() const { return success; }
    bool isEmpty() const { return tree.empty(); }
};

  // SettingsJsonParser for reading and writing JSON files
  class SettingsJsonParser {
    private:
      SettingsJsonParser() = default;
      SettingsJsonParser(const SettingsJsonParser&) = delete;
      SettingsJsonParser& operator=(const SettingsJsonParser&) = delete;

      JsonParseResult parseWithStatus(const std::string& jsonFilePath);

      // Plugin-specific validation
      uint64_t getPluginTypeFromString(const std::string& pluginName);
      std::string getPluginNameFromType(uint64_t pluginType);
      std::vector<std::string> getSupportedModules(uint64_t pluginType);
      std::vector<std::string> getSupportedSections(uint64_t pluginType);

      // Lazy initialization functions to avoid static destruction order issues
      static const std::map<uint64_t, std::vector<std::string>>& getPluginModules();
      static const std::map<uint64_t, std::vector<std::string>>& getPluginSections();
      static const std::map<std::string, std::vector<SchemaField>>& getModuleSchemasGraphBased();
      static const std::map<std::string, std::vector<SchemaField>>& getModuleSchemasTileBased();
      static const std::vector<SchemaField>& getPluginSettingsSchema();
      
      bool validatePluginSchema(const pt::ptree& tree, uint64_t pluginType);
      ValidationResult validateMetricEntry(const pt::ptree& entry, const std::string& moduleName, 
                                          const std::string& sectionType);
      ValidationResult validateField(const pt::ptree& entry, const SchemaField& field, const std::string& contextPrefix = "");
      std::string validateTileRange(const pt::ptree& entry) const;
      std::string validateTileCoordinateArray(const pt::ptree& coordArray, const std::string& fieldName) const;
      std::string getChannelArrayInfo(const pt::ptree& channelsArray) const;
      bool isValidChannelArray(const pt::ptree& channelsArray) const;
      bool isValidTileRange(const pt::ptree& entry) const;
      std::vector<SchemaField> getSchemaForModule(const std::string& moduleName, 
                                                 const std::string& sectionType) const;
      
    public:
      static SettingsJsonParser& getInstance() {
        static SettingsJsonParser instance;
        return instance;
      }

      pt::ptree parse(const std::string& jsonFilePath);
      bool isValidJson(const std::string& jsonFilePath);
      void write(const std::string& filename, const MetricCollection& collection);

      // Enhanced parsing methods
      XdpJsonSetting parseXdpJsonSetting(const std::string& jsonFilePath, uint64_t queryPluginType);
      PluginJsonSetting parsePluginJsonSetting(const pt::ptree& tree, uint64_t pluginType);
      
  };
} // namespace xdp

#endif
