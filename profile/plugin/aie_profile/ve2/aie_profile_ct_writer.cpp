// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_profile/ve2/aie_profile_ct_writer.h"
#include "xdp/profile/plugin/aie_profile/aie_profile_metadata.h"
#include "xdp/profile/database/database.h"
#include "xdp/profile/database/static_info/aie_constructs.h"

#include "core/common/message.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>
#include <vector>
#include <numeric>

namespace xdp {

using severity_level = xrt_core::message::severity_level;
namespace fs = std::filesystem;

AieProfileCTWriter::AieProfileCTWriter(VPDatabase* database,
                                       std::shared_ptr<AieProfileMetadata> metadata,
                                       uint64_t deviceId)
    : db(database)
    , metadata(metadata)
    , deviceId(deviceId)
    , columnShift(0)
    , rowShift(0)
{
  // Get column and row shift values from AIE configuration metadata
  auto config = metadata->getAIEConfigMetadata();
  columnShift = config.column_shift;
  rowShift = config.row_shift;
}

bool AieProfileCTWriter::generate()
{
  // Step 1: Read ASM file information from CSV
  std::string csvPath = (fs::current_path() / "aie_profile_timestamps.csv").string();
  auto asmFiles = readASMInfoFromCSV(csvPath);
  if (asmFiles.empty()) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "No ASM file information found in CSV. CT file will not be generated.");
    return false;
  }

  // Step 2: Get all configured counters
  auto allCounters = getConfiguredCounters();
  if (allCounters.empty()) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "No AIE counters configured. CT file will not be generated.");
    return false;
  }

  // Step 3: Filter counters for each ASM file's column range
  bool hasTimestamps = false;
  for (auto& asmFile : asmFiles) {
    if (!asmFile.timestamps.empty())
      hasTimestamps = true;

    // Filter counters for this ASM file's column range
    asmFile.counters = filterCountersByColumn(allCounters, 
                                               asmFile.colStart, 
                                               asmFile.colEnd);
  }

  if (!hasTimestamps) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "No SAVE_TIMESTAMPS instructions found in CSV. CT file will not be generated.");
    return false;
  }

  // Step 4: Generate the CT file
  return writeCTFile(asmFiles, allCounters);
}

std::vector<ASMFileInfo> AieProfileCTWriter::readASMInfoFromCSV(const std::string& csvPath)
{
  std::vector<ASMFileInfo> asmFiles;

  std::ifstream csvFile(csvPath);
  if (!csvFile.is_open()) {
    std::stringstream msg;
    msg << "Unable to open CSV file: " << csvPath << ". Please run parse_aie_runtime_to_csv.py first.";
    xrt_core::message::send(severity_level::warning, "XRT", msg.str());
    return asmFiles;
  }

  std::string line;
  bool isHeader = true;
  int lineNum = 0;
  
  // Regex pattern to extract ASM ID from filename
  std::regex filenamePattern(R"(aie_runtime_control(\d+)\.asm)");

  try {
    while (std::getline(csvFile, line)) {
      lineNum++;
      
      // Skip header
      if (isHeader) {
        isHeader = false;
        continue;
      }

      // Skip empty lines
      if (line.empty())
        continue;

      // Parse CSV line: filepath,filename,line_numbers
      // line_numbers is comma-separated like "6,8,293,439,..."
      std::vector<std::string> fields;
      std::string field;
      bool inQuote = false;
      
      for (char c : line) {
        if (c == '"') {
          inQuote = !inQuote;
        } else if (c == ',' && !inQuote) {
          fields.push_back(field);
          field.clear();
        } else {
          field += c;
        }
      }
      fields.push_back(field);  // Add last field

      // Need exactly 3 fields
      if (fields.size() != 3) {
        std::stringstream msg;
        msg << "Invalid CSV format at line " << lineNum << ": expected 3 fields, got " << fields.size();
        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        continue;
      }

      ASMFileInfo info;
      info.filename = fields[1];  // filename column
      
      // Extract ASM ID from filename
      std::smatch match;
      if (std::regex_search(info.filename, match, filenamePattern)) {
        info.asmId = std::stoi(match[1].str());
        info.ucNumber = 4 * info.asmId;
        info.colStart = info.asmId * 4;
        info.colEnd = info.colStart + 3;
      } else {
        std::stringstream msg;
        msg << "Unable to extract ASM ID from filename: " << info.filename;
        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        continue;
      }

      // Parse line numbers (comma-separated string)
      std::string lineNumbersStr = fields[2];
      std::stringstream ss(lineNumbersStr);
      std::string lineNumStr;
      
      while (std::getline(ss, lineNumStr, ',')) {
        if (!lineNumStr.empty()) {
          try {
            SaveTimestampInfo ts;
            ts.lineNumber = std::stoi(lineNumStr);
            ts.optionalIndex = -1;  // Not used in simplified format
            info.timestamps.push_back(ts);
          } catch (const std::exception& e) {
            std::stringstream msg;
            msg << "Error parsing line number '" << lineNumStr << "' in " << info.filename;
            xrt_core::message::send(severity_level::warning, "XRT", msg.str());
          }
        }
      }

      asmFiles.push_back(info);

      std::stringstream msg;
      msg << "Loaded " << info.filename << " (id=" << info.asmId 
          << ", uc=" << info.ucNumber << ", columns " << info.colStart 
          << "-" << info.colEnd << ", " << info.timestamps.size() << " timestamps)";
      xrt_core::message::send(severity_level::debug, "XRT", msg.str());
    }
  }
  catch (const std::exception& e) {
    std::stringstream msg;
    msg << "Error parsing CSV at line " << lineNum << ": " << e.what();
    xrt_core::message::send(severity_level::warning, "XRT", msg.str());
  }

  csvFile.close();

  // Sort by ASM ID for consistent output
  std::sort(asmFiles.begin(), asmFiles.end(), 
            [](const ASMFileInfo& a, const ASMFileInfo& b) {
              return a.asmId < b.asmId;
            });

  std::stringstream msg;
  msg << "Loaded " << asmFiles.size() << " ASM files from CSV with "
      << std::accumulate(asmFiles.begin(), asmFiles.end(), 0,
                        [](int sum, const ASMFileInfo& info) { 
                          return sum + info.timestamps.size(); 
                        })
      << " total SAVE_TIMESTAMPS";
  xrt_core::message::send(severity_level::info, "XRT", msg.str());

  return asmFiles;
}

std::vector<CTCounterInfo> AieProfileCTWriter::getConfiguredCounters()
{
  std::vector<CTCounterInfo> counters;

  // Get profile configuration directly from metadata to lookup metric sets for each tile
  // Note: We get it from metadata because the profile config might not be saved to database yet
  auto profileConfigPtr = metadata->createAIEProfileConfig();
  const AIEProfileFinalConfig* profileConfig = profileConfigPtr.get();

  uint64_t numCounters = db->getStaticInfo().getNumAIECounter(deviceId);
  
  for (uint64_t i = 0; i < numCounters; i++) {
    AIECounter* aieCounter = db->getStaticInfo().getAIECounter(deviceId, i);
    if (!aieCounter)
      continue;

    CTCounterInfo info;
    info.column = aieCounter->column;
    info.row = aieCounter->row;
    info.counterNumber = aieCounter->counterNumber;
    info.module = aieCounter->module;
    info.address = calculateCounterAddress(info.column, info.row, 
                                            info.counterNumber, info.module);

    // Lookup metric set for this counter's tile from profile configuration
    info.metricSet = "";
    if (profileConfig) {
      tile_type targetTile;
      targetTile.col = aieCounter->column;
      targetTile.row = aieCounter->row;
      
      // Search through all module configurations for this tile
      for (const auto& moduleMetrics : profileConfig->configMetrics) {
        for (const auto& tileMetric : moduleMetrics) {
          if (tileMetric.first.col == targetTile.col && 
              tileMetric.first.row == targetTile.row) {
            info.metricSet = tileMetric.second;
            break;
          }
        }
        if (!info.metricSet.empty())
          break;
      }
    }

    // Get port direction for throughput metrics
    if (isThroughputMetric(info.metricSet)) {
      info.portDirection = getPortDirection(info.metricSet, aieCounter->payload);
    } else {
      info.portDirection = "";
    }

    counters.push_back(info);
  }

  std::stringstream msg;
  msg << "Retrieved " << counters.size() << " configured AIE counters";
  xrt_core::message::send(severity_level::debug, "XRT", msg.str());

  return counters;
}

std::vector<CTCounterInfo> AieProfileCTWriter::filterCountersByColumn(
    const std::vector<CTCounterInfo>& allCounters,
    int colStart, int colEnd)
{
  std::vector<CTCounterInfo> filtered;

  for (const auto& counter : allCounters) {
    if (counter.column >= colStart && counter.column <= colEnd) {
      filtered.push_back(counter);
    }
  }

  return filtered;
}

uint64_t AieProfileCTWriter::calculateCounterAddress(uint8_t column, uint8_t row,
                                                      uint8_t counterNumber,
                                                      const std::string& module)
{
  // Calculate tile address from column and row
  uint64_t tileAddress = (static_cast<uint64_t>(column) << columnShift) |
                         (static_cast<uint64_t>(row) << rowShift);

  // Get base offset for module type
  uint64_t baseOffset = getModuleBaseOffset(module);

  // Counter offset (each counter is 4 bytes apart)
  uint64_t counterOffset = counterNumber * 4;

  return tileAddress + baseOffset + counterOffset;
}

uint64_t AieProfileCTWriter::getModuleBaseOffset(const std::string& module)
{
  if (module == "aie")
    return CORE_MODULE_BASE_OFFSET;
  else if (module == "aie_memory")
    return MEMORY_MODULE_BASE_OFFSET;
  else if (module == "memory_tile")
    return MEM_TILE_BASE_OFFSET;
  else if (module == "interface_tile")
    return SHIM_TILE_BASE_OFFSET;
  else
    return CORE_MODULE_BASE_OFFSET;  // Default to core module
}

std::string AieProfileCTWriter::formatAddress(uint64_t address)
{
  std::stringstream ss;
  ss << "0x" << std::hex << std::setfill('0') << std::setw(10) << address;
  return ss.str();
}

bool AieProfileCTWriter::isThroughputMetric(const std::string& metricSet)
{
  return (metricSet.find("throughput") != std::string::npos) ||
         (metricSet.find("bandwidth") != std::string::npos);
}

std::string AieProfileCTWriter::getPortDirection(const std::string& metricSet, uint64_t payload)
{
  // For interface tile ddr_bandwidth, read_bandwidth, write_bandwidth - use payload
  // These metrics can have mixed input/output ports per tile
  if (metricSet == "ddr_bandwidth" || 
      metricSet == "read_bandwidth" || 
      metricSet == "write_bandwidth") {
    constexpr uint8_t PAYLOAD_IS_MASTER_SHIFT = 8;
    bool isMaster = (payload >> PAYLOAD_IS_MASTER_SHIFT) & 0x1;
    return isMaster ? "output" : "input";
  }
  
  // For input/s2mm metrics - always input direction
  if (metricSet.find("input") != std::string::npos || 
      metricSet.find("s2mm") != std::string::npos) {
    return "input";
  }
  
  // For output/mm2s metrics - always output direction
  if (metricSet.find("output") != std::string::npos || 
      metricSet.find("mm2s") != std::string::npos) {
    return "output";
  }
  
  return "";  // Not a throughput metric with port direction
}

bool AieProfileCTWriter::writeCTFile(const std::vector<ASMFileInfo>& asmFiles,
                                      const std::vector<CTCounterInfo>& allCounters)
{
  std::string outputPath = (fs::current_path() / CT_OUTPUT_FILENAME).string();
  std::ofstream ctFile(outputPath);

  if (!ctFile.is_open()) {
    std::stringstream msg;
    msg << "Unable to create CT file: " << outputPath;
    xrt_core::message::send(severity_level::warning, "XRT", msg.str());
    return false;
  }

  // Write header comment
  ctFile << "# Auto-generated CT file for AIE Profile counters\n";
  ctFile << "# Generated by XRT AIE Profile Plugin\n\n";

  // Write begin block
  ctFile << "begin\n";
  ctFile << "{\n";
  ctFile << "    ts_start = timestamp32()\n";
  ctFile << "    print(\"\\nAIE Profile tracing started\\n\")\n";
  ctFile << "@blockopen\n";
  ctFile << "import json\n";
  ctFile << "import os\n";
  ctFile << "\n";
  ctFile << "# Initialize data collection\n";
  ctFile << "profile_data = {\n";
  ctFile << "    \"start_timestamp\": ts_start,\n";
  ctFile << "    \"counter_metadata\": [\n";

  // Write counter metadata
  for (size_t i = 0; i < allCounters.size(); i++) {
    const auto& counter = allCounters[i];
    ctFile << "        {\"column\": " << static_cast<int>(counter.column)
           << ", \"row\": " << static_cast<int>(counter.row)
           << ", \"counter\": " << static_cast<int>(counter.counterNumber)
           << ", \"module\": \"" << counter.module
           << "\", \"address\": \"" << formatAddress(counter.address) << "\"";
    
    // Add metric_set if available
    if (!counter.metricSet.empty()) {
      ctFile << ", \"metric_set\": \"" << counter.metricSet << "\"";
    }
    
    // Add port_direction for throughput metrics
    if (!counter.portDirection.empty()) {
      ctFile << ", \"port_direction\": \"" << counter.portDirection << "\"";
    }
    
    ctFile << "}";
    if (i < allCounters.size() - 1)
      ctFile << ",";
    ctFile << "\n";
  }

  ctFile << "    ],\n";
  ctFile << "    \"probes\": []\n";
  ctFile << "}\n";
  ctFile << "@blockclose\n";
  ctFile << "}\n\n";

  // Write jprobe blocks for each ASM file
  for (const auto& asmFile : asmFiles) {
    if (asmFile.timestamps.empty() || asmFile.counters.empty())
      continue;

    std::string basename = fs::path(asmFile.filename).filename().string();

    // Write comment
    ctFile << "# Probes for " << basename 
           << " (columns " << asmFile.colStart << "-" << asmFile.colEnd << ")\n";

    // Build line number list for jprobe
    std::stringstream lineList;
    lineList << "line";
    for (size_t i = 0; i < asmFile.timestamps.size(); i++) {
      if (i > 0)
        lineList << ",";
      lineList << asmFile.timestamps[i].lineNumber;
    }

    // Write jprobe declaration
    ctFile << "jprobe:" << basename 
           << ":uc" << asmFile.ucNumber 
           << ":" << lineList.str() << "\n";
    ctFile << "{\n";
    ctFile << "    ts = timestamp32()\n";

    // Write counter reads
    for (size_t i = 0; i < asmFile.counters.size(); i++) {
      ctFile << "    ctr_" << i << " = read_reg(" 
             << formatAddress(asmFile.counters[i].address) << ")\n";
    }

    // Group counters by tile (col, row)
    std::map<std::pair<uint8_t, uint8_t>, std::vector<size_t>> tileCounters;
    for (size_t i = 0; i < asmFile.counters.size(); i++) {
      auto key = std::make_pair(asmFile.counters[i].column, asmFile.counters[i].row);
      tileCounters[key].push_back(i);
    }

    ctFile << "    print(f\"Probe fired: ts={ts}\")\n";
    ctFile << "@blockopen\n";
    ctFile << "profile_data[\"probes\"].append({\n";
    ctFile << "    \"asm_file\": \"" << basename << "\",\n";
    ctFile << "    \"timestamp\": ts,\n";
    ctFile << "    \"tiles\": [\n";

    // Write tile groups with their counters
    size_t tileIdx = 0;
    for (const auto& tilePair : tileCounters) {
      const auto& tile = tilePair.first;
      const auto& counterIndices = tilePair.second;
      
      ctFile << "        {\"col\": " << static_cast<int>(tile.first)
             << ", \"row\": " << static_cast<int>(tile.second)
             << ", \"counters\": [";
      
      for (size_t j = 0; j < counterIndices.size(); j++) {
        if (j > 0)
          ctFile << ", ";
        ctFile << "ctr_" << counterIndices[j];
      }
      
      ctFile << "]}";
      if (tileIdx < tileCounters.size() - 1)
        ctFile << ",";
      ctFile << "\n";
      tileIdx++;
    }

    ctFile << "    ]\n";
    ctFile << "})\n";
    ctFile << "@blockclose\n";
    ctFile << "}\n\n";
  }

  // Write end block
  ctFile << "end\n";
  ctFile << "{\n";
  ctFile << "    ts_end = timestamp32()\n";
  ctFile << "    print(\"\\nAIE Profile tracing ended\\n\")\n";
  ctFile << "@blockopen\n";
  ctFile << "profile_data[\"end_timestamp\"] = ts_end\n";
  ctFile << "profile_data[\"total_time\"] = ts_end - profile_data[\"start_timestamp\"]\n";
  ctFile << "\n";
  ctFile << "output_path = os.path.join(os.getcwd(), \"aie_profile_counters.json\")\n";
  ctFile << "with open(output_path, \"w\") as f:\n";
  ctFile << "    json.dump(profile_data, f, indent=2)\n";
  ctFile << "print(f\"Profile data written to {output_path}\")\n";
  ctFile << "@blockclose\n";
  ctFile << "}\n";

  ctFile.close();

  std::stringstream msg;
  msg << "Generated CT file: " << outputPath;
  xrt_core::message::send(severity_level::info, "XRT", msg.str());

  return true;
}

} // namespace xdp

