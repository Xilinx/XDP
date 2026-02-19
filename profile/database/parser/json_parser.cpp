#include <unordered_map>
#include <filesystem>
#include "json_parser.h"

namespace xdp {

    void SettingsJsonParser::write(const std::string& filename, const MetricCollection& collection) {
        std::ofstream file(filename);
        if (!file) {
            throw std::runtime_error("Error writing to file: " + filename);
        }

        // Print collection 
        // std::cout << "!!! After: Writing MetricCollection to JSON file: " << filename << std::endl;
        // Print all metrics for debugging purposes
        for (const auto& metric : collection.metrics) {
            if (metric) {
                metric->print(); // Call the print method of each metric
            } else {
                xrt_core::message::send(severity_level::warning, "XRT", "Null metric found in collection");
            }
        }
      
        boost::property_tree::ptree pt = collection.toPtree();
        boost::property_tree::write_json(file, pt);
    }

    // Implementation
    JsonParseResult SettingsJsonParser::parseWithStatus(const std::string& jsonFilePath) {
        JsonParseResult result;
        
        // Check if file exists
        if (!std::filesystem::exists(jsonFilePath)) {
            result.errorMessage = "File not found: " + jsonFilePath;
            xrt_core::message::send(severity_level::info, "XRT", 
                result.errorMessage + ", proceeding with default settings.");
            return result;
        }
        
        // Check if it's a regular file
        if (!std::filesystem::is_regular_file(jsonFilePath)) {
            result.errorMessage = "Path exists but is not a regular file: " + jsonFilePath;
            xrt_core::message::send(severity_level::warning, "XRT", result.errorMessage);
            return result;
        }
        
        std::ifstream jsonFile(jsonFilePath);
        if (!jsonFile.is_open()) {
            result.errorMessage = "Failed to open file: " + jsonFilePath;
            xrt_core::message::send(severity_level::warning, "XRT", result.errorMessage);
            return result;
        }
        
        try {
            boost::property_tree::read_json(jsonFile, result.tree);
            result.success = true;
            // xrt_core::message::send(severity_level::info, "XRT", 
            //     "Successfully parsed JSON file: " + jsonFilePath);
        } catch (const pt::json_parser_error& e) {
            result.errorMessage = "JSON parse error: " + std::string(e.what());
            xrt_core::message::send(severity_level::error, "XRT", 
                "Failed to parse JSON file '" + jsonFilePath + "': " + result.errorMessage);
            result.tree.clear();
        }
        return result;
    }
    
    // Backward compatible version
    pt::ptree SettingsJsonParser::parse(const std::string& jsonFilePath) {
        return parseWithStatus(jsonFilePath).tree;
    }

    bool SettingsJsonParser::isValidJson(const std::string& jsonFilePath) {
        if (jsonFilePath.empty())
            return false;
        try {
            return parseWithStatus(jsonFilePath).isValid();
        } catch (const std::exception& e) {
            xrt_core::message::send(severity_level::warning, "XRT", 
                "Invalid JSON file: " + std::string(e.what()));
            return false;
        }
    }

    // Lazy initialization functions to avoid static destruction order issues
    const std::map<uint64_t, std::vector<std::string>>& SettingsJsonParser::getPluginModules() {
        static const std::map<uint64_t, std::vector<std::string>> pluginModules = {
            {info::aie_profile, {"aie", "aie_memory", "interface_tile", "memory_tile", "microcontroller"}},
            {info::aie_trace,   {"aie_tile", "interface_tile", "memory_tile"}}
        };
        return pluginModules;
    }

    const std::map<uint64_t, std::vector<std::string>>& SettingsJsonParser::getPluginSections() {
        static const std::map<uint64_t, std::vector<std::string>> pluginSections = {
            {info::aie_profile, {"tiles", "graphs"}},
            {info::aie_trace,   {"tiles", "graphs"}}
        };
        return pluginSections;
    }

    // Lazy initialization functions to avoid static destruction order issues
  const std::map<std::string, std::vector<SchemaField>>& SettingsJsonParser::getModuleSchemasGraphBased() {
    static const std::map<std::string, std::vector<SchemaField>> schemas = {
        {"aie", {
            SchemaField("graph", true, "string"),
            SchemaField("kernel", true, "string"),
            SchemaField("metric", true, "string"),
            SchemaField("channels", false, "array")
        }},
        {"aie_memory", {
            SchemaField("graph", true, "string"),
            SchemaField("kernel", true, "string"),
            SchemaField("metric", true, "string"),
            SchemaField("channels", false, "array")
        }},
        {"aie_tile", {
            SchemaField("graph", true, "string"),
            SchemaField("kernel", true, "string"),
            SchemaField("metric", true, "string"),
            SchemaField("channels", false, "array")
        }},
        {"memory_tile", {
            SchemaField("graph", true, "string"),
            SchemaField("buffer", true, "string"),
            SchemaField("metric", true, "string"),
            SchemaField("channels", false, "array")
        }},
        {"interface_tile", {
            SchemaField("graph", true, "string"),
            SchemaField("port", true, "string"),
            SchemaField("metric", true, "string"),
            SchemaField("channels", false, "array"),
            SchemaField("bytes", false, "string")
        }},
        {"microcontroller", {
            SchemaField("graph", true, "string"),
            SchemaField("metric", true, "string")
        }}
    };
    return schemas;
  }

  const std::map<std::string, std::vector<SchemaField>>& SettingsJsonParser::getModuleSchemasTileBased() {
    static const std::map<std::string, std::vector<SchemaField>> schemas = {
        {"aie", {
            SchemaField("metric", true, "string"),
            SchemaField("col", false, "int"),
            SchemaField("row", false, "int"),
            SchemaField("all_tiles", false, "bool"),
            SchemaField("start", false, "array"),
            SchemaField("end", false, "array"),
            SchemaField("channels", false, "array")
        }},
        {"aie_memory", {
            SchemaField("metric", true, "string"),
            SchemaField("col", false, "int"),
            SchemaField("row", false, "int"),
            SchemaField("all_tiles", false, "bool"),
            SchemaField("start", false, "array"),
            SchemaField("end", false, "array"),
            SchemaField("channels", false, "array")
        }},
        {"aie_tile", {
            SchemaField("metric", true, "string"),
            SchemaField("col", false, "int"),
            SchemaField("row", false, "int"),
            SchemaField("all_tiles", false, "bool"),
            SchemaField("start", false, "array"),
            SchemaField("end", false, "array"),
            SchemaField("channels", false, "array")
        }},
        {"memory_tile", {
            SchemaField("metric", true, "string"),
            SchemaField("col", false, "int"),
            SchemaField("row", false, "int"),
            SchemaField("all_tiles", false, "bool"),
            SchemaField("start", false, "array"),
            SchemaField("end", false, "array"),
            SchemaField("channels", false, "array")
        }},
        {"interface_tile", {
            SchemaField("metric", true, "string"),
            SchemaField("col", false, "int"),
            SchemaField("row", false, "int"),
            SchemaField("all_tiles", false, "bool"),
            SchemaField("start", false, "array"),
            SchemaField("end", false, "array"),
            SchemaField("channels", false, "array"),
            SchemaField("bytes", false, "string")
        }},
        {"microcontroller", {
            SchemaField("metric", true, "string"),
            SchemaField("col", false, "int"),
            SchemaField("row", false, "int"),
            SchemaField("all_tiles", false, "bool"),
            SchemaField("start", false, "array"),
            SchemaField("end", false, "array")
        }}
    };
    return schemas;
  }

  const std::vector<SchemaField>& SettingsJsonParser::getPluginSettingsSchema() {
    static const std::vector<SchemaField> schema = {
        SchemaField("interval_us", false, "int"),
        SchemaField("start_type", false, "string", {"time", "iteration"}),
        SchemaField("start_iteration", false, "int")
    };
    return schema;
  }

    XdpJsonSetting SettingsJsonParser::parseXdpJsonSetting(const std::string& jsonFilePath,
                                         uint64_t queryPluginType)
    {
        XdpJsonSetting config;
        
        try {
            pt::ptree jsonTree = parse(jsonFilePath);
            
            // Check for version field (optional)
            auto versionOpt = jsonTree.get_optional<std::string>("version");
            if (versionOpt) {
                config.version = *versionOpt;
                xrt_core::message::send(severity_level::info, "XRT", 
                    "JSON configuration version: " + config.version);
            }
            
            // Parse each plugin section
            for (const auto& [pluginName, pluginTree] : jsonTree) {
                // Skip version field - it's handled separately
                if (pluginName == "version") {
                    continue;
                }
                
                uint64_t pluginType = getPluginTypeFromString(pluginName);
                
                if (pluginType == 0) {
                  std::stringstream msg;
                  msg << "Unknown plugin name specified: " << pluginName;
                  config.errorMessage = msg.str();
                  xrt_core::message::send(severity_level::warning, "XRT", msg.str());
                    continue;
                }
                // Skip if plugin type does not match
                if (pluginType != queryPluginType) {
                  std::string queryPluginName = (queryPluginType == info::aie_profile) ? "aie_profile" : 
                                               (queryPluginType == info::aie_trace) ? "aie_trace" : "unknown";
                  xrt_core::message::send(severity_level::debug, "XRT", 
                                      "Skipping " + pluginName + " settings for " + queryPluginName + " query");
                  continue;
                }
                
                PluginJsonSetting pluginSettings = parsePluginJsonSetting(pluginTree, pluginType);
                if (pluginSettings.isValid) {
                    config.plugins[pluginType] = std::move(pluginSettings);
                } else {
                    xrt_core::message::send(severity_level::error, "XRT", 
                        "Failed to parse " + pluginName + ": " + pluginSettings.errorMessage);
                }
            }
            
            config.isValid = !config.plugins.empty();
            
        } catch (const std::exception& e) {
            config.errorMessage = "Parse error: " + std::string(e.what());
        }
        
        return config;
    }

    PluginJsonSetting SettingsJsonParser::parsePluginJsonSetting(const pt::ptree& tree, uint64_t pluginType) {
        PluginJsonSetting config;
        config.pluginType = pluginType;
        
        try {
            auto supportedSections = getSupportedSections(pluginType);
            auto supportedModules  = getSupportedModules(pluginType);

            // Parse plugin-level settings first
            for (const auto& field : getPluginSettingsSchema()) {
                auto fieldOpt = tree.get_optional<std::string>(field.name);
                if (fieldOpt) {
                    if (field.name == "interval_us") {
                        try {
                            uint32_t intervalUs = tree.get<uint32_t>(field.name);
                            config.settings.intervalUs = intervalUs;
                            xrt_core::message::send(severity_level::debug, "XRT", 
                                "Found plugin setting interval_us: " + std::to_string(intervalUs));
                        } catch (const std::exception&) {
                            xrt_core::message::send(severity_level::warning, "XRT", 
                                "Invalid interval_us value: " + *fieldOpt);
                        }
                    } else if (field.name == "start_type") {
                        if (std::find(field.allowedValues.begin(), field.allowedValues.end(), *fieldOpt) != field.allowedValues.end()) {
                            config.settings.startType = *fieldOpt;
                            xrt_core::message::send(severity_level::debug, "XRT", 
                                "Found plugin setting start_type: " + *fieldOpt);
                        } else {
                            xrt_core::message::send(severity_level::warning, "XRT", 
                                "Invalid start_type value: " + *fieldOpt + ". Must be 'time' or 'iteration'");
                        }
                    } else if (field.name == "start_iteration") {
                        try {
                            uint32_t startIteration = tree.get<uint32_t>(field.name);
                            config.settings.startIteration = startIteration;
                            xrt_core::message::send(severity_level::debug, "XRT", 
                                "Found plugin setting start_iteration: " + std::to_string(startIteration));
                        } catch (const std::exception&) {
                            xrt_core::message::send(severity_level::warning, "XRT", 
                                "Invalid start_iteration value: " + *fieldOpt);
                        }
                    }
                }
            }

            // Track modules to detect conflicts - map module to first section it appears in
            // PLUGIN_MODULES -> sectionName ("tiles", "graphs")
            std::unordered_map<std::string, std::string> moduleToFirstSection;
            
            // Parse sections (tiles, graphs)
            for (const auto& [sectionKey, section] : tree) {
                
                if (std::find(supportedSections.begin(), supportedSections.end(), sectionKey) == supportedSections.end()) {
                    // Skip plugin-level settings - they are not sections
                    bool isPluginSetting = false;
                    for (const auto& field : getPluginSettingsSchema()) {
                        if (sectionKey == field.name) {
                            isPluginSetting = true;
                            break;
                        }
                    }
                    if (isPluginSetting) {
                        continue; // Skip plugin-level settings
                    }
                    
                    xrt_core::message::send(severity_level::warning, "XRT", 
                        "Unsupported section for this plugin: " + sectionKey);
                    continue;
                }
                
                // Parse modules within section
                for (const auto& [moduleKey, moduleArray] : section) {
                    if (std::find(supportedModules.begin(), supportedModules.end(), moduleKey) == supportedModules.end()) {
                        xrt_core::message::send(severity_level::warning, "XRT", 
                            "Unsupported module for this plugin: " + moduleKey);
                        continue;
                    }

                    // Validate each metric entry
                    std::vector<pt::ptree> metrics;
                    for (const auto& item : moduleArray) {
                        ValidationResult result = validateMetricEntry(item.second, moduleKey, sectionKey);
                        for (const auto& error : result.errors) {
                            xrt_core::message::send(severity_level::error, "XRT", 
                                "JSON schema error in module " + moduleKey + ": " + error);
                        }
                        for (const auto& warning : result.warnings) {
                            xrt_core::message::send(severity_level::warning, "XRT", 
                                "JSON schema warning in module " + moduleKey + ": " + warning);
                        }
                        if (!result.isValid) {
                          continue;
                        }

                        // Check for conflict - same module in different section
                        if (moduleToFirstSection.find(moduleKey) != moduleToFirstSection.end()) {
                          std::string firstSection = moduleToFirstSection[moduleKey];
                          if (firstSection != sectionKey) {
                              std::stringstream warningMsg;
                              warningMsg << "Warning: Module '" << moduleKey  << "' appears in both '"
                                        << firstSection + "' and '" + sectionKey
                                        << "' sections. Using configuration from '" << firstSection
                                        << "' section and ignoring '" + sectionKey + "' configuration.";
                              xrt_core::message::send(severity_level::warning, "XRT", warningMsg.str());
                              continue; // Skip this duplicate module in later section
                          }
                        } else {
                          // First time seeing this module - record which section it's in
                          moduleToFirstSection[moduleKey] = sectionKey;
                        }
                        
                        metrics.push_back(item.second);
                     } // end for each metric in moduleArray
                     
                     if (metrics.empty()) {
                        xrt_core::message::send(severity_level::warning, "XRT", 
                            "No valid metrics found for module: " + moduleKey + " in plugin: " + getPluginNameFromType(pluginType));
                        continue;
                     }
                     config.sections[sectionKey][moduleKey] = metrics;
                     config.isValid = true;
                } // end for each module in section
            } // end for each section (tiles, graphs)
        } catch (const std::exception& e) {
            config.errorMessage = "Parse error: " + std::string(e.what());
        }
        
        return config;
    }

    uint64_t SettingsJsonParser::getPluginTypeFromString(const std::string& pluginName) {
        if (pluginName == "aie_profile") return info::aie_profile;
        if (pluginName == "aie_trace") return info::aie_trace;
        return 0; // Unknown plugin type
    }

    std::string SettingsJsonParser::getPluginNameFromType(uint64_t pluginType) {
        if (pluginType == info::aie_profile) return "aie_profile";
        if (pluginType == info::aie_trace) return "aie_trace";
        return "unknown";
    }

    std::vector<std::string> SettingsJsonParser::getSupportedModules(uint64_t pluginType) {
        const auto& pluginModules = getPluginModules();
        auto it = pluginModules.find(pluginType);
        return (it != pluginModules.end()) ? it->second : std::vector<std::string>{};
    }

    std::vector<std::string> SettingsJsonParser::getSupportedSections(uint64_t pluginType) {
        const auto& pluginSections = getPluginSections();
        auto it = pluginSections.find(pluginType);
        return (it != pluginSections.end()) ? it->second : std::vector<std::string>{};
    }

  ValidationResult SettingsJsonParser::validateMetricEntry(const pt::ptree& entry, const std::string& moduleName, 
                                                           const std::string& sectionType) {
    ValidationResult result;
    
    // Get metric name for better error context
    std::string metricName = entry.get<std::string>("metric", "<unknown>");
    std::string contextPrefix = "metric '" + metricName + "' in module '" + moduleName + "': ";
    
    auto schema = getSchemaForModule(moduleName, sectionType);
    
    for (const auto& field : schema) {
        ValidationResult fieldResult = validateField(entry, field, contextPrefix);
        result.errors.insert(result.errors.end(), fieldResult.errors.begin(), fieldResult.errors.end());
        result.warnings.insert(result.warnings.end(), fieldResult.warnings.begin(), fieldResult.warnings.end());
        if (!fieldResult.isValid) {
            result.isValid = false;
        }
    }
    
    // Additional validation for tile-based entries
    if (sectionType == "tiles") {
        // Check that we have valid tile specification
        bool hasAllTiles = entry.get_optional<bool>("all_tiles").value_or(false);
        bool hasColRow = entry.get_optional<int>("col") && entry.get_optional<int>("row");
        bool hasRange = entry.get_child_optional("start") && entry.get_child_optional("end");
        bool hasSingleCol = entry.get_optional<int>("col") && !entry.get_optional<int>("row");
        
        // Build context string for tile specification
        std::string tileSpec = "not specified";
        if (hasAllTiles) {
            tileSpec = "all_tiles=true";
        } else if (hasColRow) {
            tileSpec = "col=" + std::to_string(entry.get<int>("col")) + ", row=" + std::to_string(entry.get<int>("row"));
        } else if (hasSingleCol) {
            tileSpec = "col=" + std::to_string(entry.get<int>("col")) + " (row missing)";
        } else if (hasRange) {
            tileSpec = "start/end range specified";
        }
        
        // For microcontroller, only col or all_tiles is needed
        if (moduleName == "microcontroller") {
            if (!hasAllTiles && !hasSingleCol && !hasRange) {
                result.addError(contextPrefix + "tile specification required for microcontroller (current: " + tileSpec + 
                              "). Use either 'all_tiles': true, 'col': <num>, or 'start'/'end' range");
            }
        } else {
            // For other modules, need proper tile specification
            if (!hasAllTiles && !hasColRow && !hasRange) {
                result.addError(contextPrefix + "complete tile specification required (current: " + tileSpec + 
                              "). Use either 'all_tiles': true, 'col'/'row' pair, or 'start'/'end' range");
            }
        }
        
        // Validate tile range if present with detailed error reporting
        if (hasRange) {
            std::string rangeError = validateTileRange(entry);
            if (!rangeError.empty()) {
                result.addError(contextPrefix + rangeError);
            }
        }
    }
    
    return result;
}

ValidationResult SettingsJsonParser::validateField(const pt::ptree& entry, const SchemaField& field, const std::string& contextPrefix) {
    ValidationResult result;
    
    auto fieldOpt = entry.get_child_optional(field.name);
    
    if (field.required && fieldOpt == boost::none) {
        result.addError(contextPrefix + "required field '" + field.name + "' is missing");
        result.isValid = false;
        return result;
    }
    
    if (fieldOpt == boost::none) {
        return result; // Optional field not present
    }
    
    try {
        if (field.type == "string") {
            std::string value = fieldOpt->get_value<std::string>();
            // Check allowed values if specified
            if (!field.allowedValues.empty()) {
                if (std::find(field.allowedValues.begin(), field.allowedValues.end(), value) == field.allowedValues.end()) {
                    result.addError(contextPrefix + "invalid value '" + value + "' for field '" + field.name + "'");
                }
            }
        } else if (field.type == "int") {
            (void)fieldOpt->get_value<int>();
            // For debugging, we could add the actual value to the context if there's an error
        } else if (field.type == "bool") {
            fieldOpt->get_value<bool>();
        } else if (field.type == "array") {
            if (field.name == "channels") {
                if (!isValidChannelArray(*fieldOpt)) {
                    // Get detailed channel info for error message
                    std::string channelInfo = getChannelArrayInfo(*fieldOpt);
                    result.addError(contextPrefix + "invalid channels array format: " + channelInfo);
                }
            } else if (field.name == "start" || field.name == "end") {
                // Validate tile coordinate array with detailed error
                std::string coordError = validateTileCoordinateArray(*fieldOpt, field.name);
                if (!coordError.empty()) {
                    result.addError(contextPrefix + coordError);
                }
            }
        }
    } catch (const std::exception& e) {
        result.addError(contextPrefix + "invalid value for field '" + field.name + "': " + e.what());
        result.isValid = false;
    }
    
    return result;
}

bool SettingsJsonParser::isValidChannelArray(const pt::ptree& channelsArray) const {
    try {
        for (const auto& channelNode : channelsArray) {
            int channel = channelNode.second.get_value<int>();
            if (channel < 0 || channel > 255) {
                return false;
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<SchemaField> SettingsJsonParser::getSchemaForModule(const std::string& moduleName, 
                                                               const std::string& sectionType) const {
    const auto& schemas = (sectionType == "graphs") ? getModuleSchemasGraphBased() : getModuleSchemasTileBased();
    auto it = schemas.find(moduleName);
    return (it != schemas.end()) ? it->second : std::vector<SchemaField>{};
}

std::string SettingsJsonParser::validateTileRange(const pt::ptree& entry) const {
    try {
        auto startOpt = entry.get_child_optional("start");
        auto endOpt = entry.get_child_optional("end");
        
        if (!startOpt || !endOpt) {
            return "tile range requires both 'start' and 'end' arrays";
        }
        
        if (startOpt->size() != 2) {
            return "start array must contain exactly 2 elements [col, row], found " + std::to_string(startOpt->size()) + " elements";
        }
        
        if (endOpt->size() != 2) {
            return "end array must contain exactly 2 elements [col, row], found " + std::to_string(endOpt->size()) + " elements";
        }
        
        // Get coordinates
        std::vector<int> startCoords, endCoords;
        try {
            for (const auto& coord : *startOpt) {
                startCoords.push_back(coord.second.get_value<int>());
            }
            for (const auto& coord : *endOpt) {
                endCoords.push_back(coord.second.get_value<int>());
            }
        } catch (const std::exception& e) {
            return "tile coordinates must be integers: " + std::string(e.what());
        }
        
        // Validate coordinates are non-negative
        if (startCoords[0] < 0 || startCoords[1] < 0) {
            return "start coordinates must be non-negative, found start=[" + std::to_string(startCoords[0]) + ", " + std::to_string(startCoords[1]) + "]";
        }
        
        if (endCoords[0] < 0 || endCoords[1] < 0) {
            return "end coordinates must be non-negative, found end=[" + std::to_string(endCoords[0]) + ", " + std::to_string(endCoords[1]) + "]";
        }
        
        // Validate range (start <= end)
        if (startCoords[0] > endCoords[0] || startCoords[1] > endCoords[1]) {
            return "invalid tile range: start=[" + std::to_string(startCoords[0]) + ", " + std::to_string(startCoords[1]) + 
                   "] must be <= end=[" + std::to_string(endCoords[0]) + ", " + std::to_string(endCoords[1]) + "]";
        }
        
        return ""; // No error
        
    } catch (const std::exception& e) {
        return "error parsing tile range: " + std::string(e.what());
    }
}

std::string SettingsJsonParser::validateTileCoordinateArray(const pt::ptree& coordArray, const std::string& fieldName) const {
    try {
        if (coordArray.size() != 2) {
            return fieldName + " array must contain exactly 2 elements [col, row], found " + std::to_string(coordArray.size()) + " elements";
        }
        
        std::vector<int> coords;
        try {
            for (const auto& coord : coordArray) {
                coords.push_back(coord.second.get_value<int>());
            }
        } catch (const std::exception& e) {
            return fieldName + " coordinates must be integers: " + std::string(e.what());
        }
        
        // Validate coordinates are non-negative
        if (coords[0] < 0 || coords[1] < 0) {
            return fieldName + " coordinates must be non-negative, found [" + std::to_string(coords[0]) + ", " + std::to_string(coords[1]) + "]";
        }
        
        return ""; // No error
        
    } catch (const std::exception& e) {
        return "error parsing " + fieldName + " coordinates: " + std::string(e.what());
    }
}

std::string SettingsJsonParser::getChannelArrayInfo(const pt::ptree& channelsArray) const {
    std::stringstream info;
    
    try {
        info << "found " << channelsArray.size() << " channel(s): [";
        
        bool first = true;
        for (const auto& channelNode : channelsArray) {
            if (!first) info << ", ";
            first = false;
            
            try {
                int channel = channelNode.second.get_value<int>();
                info << channel;
                
                // Check for invalid values and note them
                if (channel < 0 || channel > 255) {
                    info << "(invalid)";
                }
            } catch (const std::exception&) {
                info << "non-integer";
            }
        }
        
        info << "]. Channels must be integers between 0 and 255";
        
    } catch (const std::exception& e) {
        info << "error reading channels array: " << e.what();
    }
    
    return info.str();
}

bool SettingsJsonParser::isValidTileRange(const pt::ptree& entry) const {
    return validateTileRange(entry).empty();
}

};
