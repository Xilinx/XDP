// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/ve2/aie_dtrace_ct_writer.h"
#include "xdp/profile/plugin/aie_profile/aie_profile_metadata.h"
#include "xdp/profile/database/database.h"
#include "xdp/profile/database/static_info/aie_constructs.h"

#include "core/common/message.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>
#include <vector>
#include <numeric>

namespace xdp {

namespace {

// Order UCs by aiebu min column; each UC's width is [colStart, nextUcStart - 1] (last UC ends at opLocMaxCol).
void
applyUcSpansFromOpLoc(std::vector<ASMFileInfo>& asmFiles)
{
  if (asmFiles.empty())
    return;

  std::sort(asmFiles.begin(), asmFiles.end(),
            [](const ASMFileInfo& a, const ASMFileInfo& b) {
              if (a.opLocMinCol != b.opLocMinCol)
                return a.opLocMinCol < b.opLocMinCol;
              return a.filename < b.filename;
            });

  const size_t n = asmFiles.size();
  for (size_t i = 0; i < n; ++i) {
    auto& af = asmFiles[i];
    af.colStart = static_cast<int>(af.opLocMinCol);
    af.ucNumber = af.colStart;
    if (i + 1 < n) {
      const int nextStart = static_cast<int>(asmFiles[i + 1].opLocMinCol);
      af.colEnd = nextStart - 1;
      if (af.colEnd < af.colStart)
        af.colEnd = static_cast<int>(af.opLocMaxCol);
    } else {
      af.colEnd = static_cast<int>(af.opLocMaxCol);
    }
  }
}

// Last UC spans through the rightmost column that has a configured counter (op_loc may only
// list columns where SAVE_TIMESTAMPS appears, so colEnd would otherwise stop at opLocMaxCol).
void
extendLastUcToMaxConfiguredColumn(std::vector<ASMFileInfo>& asmFiles,
                                  const std::vector<CTCounterInfo>& allCounters)
{
  if (asmFiles.empty() || allCounters.empty())
    return;

  int maxCfgCol = -1;
  for (const auto& c : allCounters)
    maxCfgCol = std::max(maxCfgCol, static_cast<int>(c.column));
  if (maxCfgCol < 0)
    return;

  auto& last = asmFiles.back();
  if (maxCfgCol >= last.colStart)
    last.colEnd = std::max(last.colEnd, maxCfgCol);
}

} // namespace

using severity_level = xrt_core::message::severity_level;
namespace fs = std::filesystem;

AieDtraceCTWriter::AieDtraceCTWriter(VPDatabase* database,
                                       std::shared_ptr<AieProfileMetadata> metadata,
                                       uint64_t deviceId,
                                       uint8_t startCol)
    : db(database)
    , metadata(metadata)
    , deviceId(deviceId)
    , columnShift(0)
    , rowShift(0)
    , partitionStartCol(startCol)
{
  auto config = metadata->getAIEConfigMetadata();
  columnShift = config.column_shift;
  rowShift = config.row_shift;
}

bool AieDtraceCTWriter::generate()
{
  return generate((fs::current_path() / CT_OUTPUT_FILENAME).string());
}

bool AieDtraceCTWriter::generate(const std::string& outputPath,
    const std::vector<aiebu::aiebu_assembler::op_loc>& opLocations)
{
  if (opLocations.empty())
    return false;

  // Convert op_loc data to ASMFileInfo structures
  std::vector<ASMFileInfo> asmFiles;
  std::regex filenamePattern(R"(aie_runtime_control(\d+)?\.asm)");

  for (const auto& loc : opLocations) {
    for (const auto& li : loc.line_info) {
      if (li.entries.empty())
        continue;

      // Use the filename from the first entry of this column group
      const auto& fname = li.entries.front().second;
      std::smatch match;
      if (!std::regex_search(fname, match, filenamePattern))
        continue;

      // Check if we already have an ASMFileInfo for this filename
      auto it = std::find_if(asmFiles.begin(), asmFiles.end(),
          [&fname](const ASMFileInfo& a) { return a.filename == fname; });

      if (it == asmFiles.end()) {
        ASMFileInfo info;
        info.filename = fname;
        info.asmId = match[1].matched ? std::stoi(match[1].str()) : 0;
        info.opLocMinCol = li.col;
        info.opLocMaxCol = li.col;
        asmFiles.push_back(info);
        it = asmFiles.end() - 1;
      } else {
        it->opLocMinCol = std::min(it->opLocMinCol, li.col);
        it->opLocMaxCol = std::max(it->opLocMaxCol, li.col);
      }

      for (const auto& entry : li.entries) {
        SaveTimestampInfo ts;
        ts.lineNumber = entry.first;
        ts.optionalIndex = -1;
        it->timestamps.push_back(ts);
      }
    }
  }

  if (asmFiles.empty())
    return false;

  applyUcSpansFromOpLoc(asmFiles);

  auto allCounters = getConfiguredCounters();
  if (allCounters.empty())
    return false;

  extendLastUcToMaxConfiguredColumn(asmFiles, allCounters);

  for (auto& asmFile : asmFiles) {
    asmFile.counters = filterCountersByColumn(allCounters,
                                               asmFile.colStart, asmFile.colEnd);
  }

  return writeCTFile(asmFiles, allCounters, outputPath);
}

bool AieDtraceCTWriter::generate(const std::string& outputPath)
{
  std::string csvPath = (fs::current_path() / "aie_profile_timestamps.csv").string();
  auto asmFiles = readASMInfoFromCSV(csvPath);
  if (asmFiles.empty()) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "No ASM file information found in CSV. CT file will not be generated.");
    return false;
  }

  auto allCounters = getConfiguredCounters();
  if (allCounters.empty()) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "No AIE counters configured. CT file will not be generated.");
    return false;
  }

  extendLastUcToMaxConfiguredColumn(asmFiles, allCounters);

  bool hasTimestamps = false;
  for (auto& asmFile : asmFiles) {
    if (!asmFile.timestamps.empty())
      hasTimestamps = true;

    asmFile.counters = filterCountersByColumn(allCounters, 
                                               asmFile.colStart, 
                                               asmFile.colEnd);
  }

  if (!hasTimestamps) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "No SAVE_TIMESTAMPS instructions found in CSV. CT file will not be generated.");
    return false;
  }

  return writeCTFile(asmFiles, allCounters, outputPath);
}

std::vector<ASMFileInfo> AieDtraceCTWriter::readASMInfoFromCSV(const std::string& csvPath)
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
  std::regex filenamePattern(R"(aie_runtime_control(\d+)?\.asm)");

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
        info.asmId = match[1].matched ? std::stoi(match[1].str()) : 0;
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

  // Sort by UC start column for consistent output
  std::sort(asmFiles.begin(), asmFiles.end(), 
            [](const ASMFileInfo& a, const ASMFileInfo& b) {
              if (a.colStart != b.colStart)
                return a.colStart < b.colStart;
              return a.filename < b.filename;
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

std::vector<CTCounterInfo> AieDtraceCTWriter::getConfiguredCounters()
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

std::vector<CTCounterInfo> AieDtraceCTWriter::filterCountersByColumn(
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

uint64_t AieDtraceCTWriter::calculateCounterAddress(uint8_t column, uint8_t row,
                                                      uint8_t counterNumber,
                                                      const std::string& module)
{
  // Use the partition-relative column directly so that CT addresses remain
  // relative to the partition's start column.
  uint64_t tileAddress = (static_cast<uint64_t>(column) << columnShift) |
                         (static_cast<uint64_t>(row) << rowShift);

  // Get base offset for module type
  uint64_t baseOffset = getModuleBaseOffset(module);

  // Counter offset (each counter is 4 bytes apart)
  uint64_t counterOffset = counterNumber * 4;

  return tileAddress + baseOffset + counterOffset;
}

uint64_t AieDtraceCTWriter::getModuleBaseOffset(const std::string& module)
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

std::string AieDtraceCTWriter::formatAddress(uint64_t address)
{
  std::stringstream ss;
  ss << "0x" << std::hex << std::setfill('0') << std::setw(10) << address;
  return ss.str();
}

bool AieDtraceCTWriter::isThroughputMetric(const std::string& metricSet)
{
  return (metricSet.find("throughput") != std::string::npos) ||
         (metricSet.find("bandwidth") != std::string::npos);
}

std::string AieDtraceCTWriter::getPortDirection(const std::string& metricSet, uint64_t payload)
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

bool AieDtraceCTWriter::writeCTFile(const std::vector<ASMFileInfo>& asmFiles,
                                      const std::vector<CTCounterInfo>& allCounters,
                                      const std::string& outputPath)
{
  std::ofstream ctFile(outputPath);

  if (!ctFile.is_open()) {
    std::stringstream msg;
    msg << "Unable to create CT file: " << outputPath;
    xrt_core::message::send(severity_level::warning, "XRT", msg.str());
    return false;
  }

  // Write header comment
  ctFile << "# Auto-generated CT file for AIE Dtrace counters\n";
  ctFile << "# Generated by XRT AIE Dtrace Plugin\n";
  ctFile << "# Counter metadata is embedded in the begin block (# COUNTER_METADATA_BEGIN/END)\n\n";

  // Write begin block with embedded counter metadata
  ctFile << "begin\n";
  ctFile << "{\n";
  ctFile << "    ts_start = timestamp32()\n";
  ctFile << "@blockopen\n";
  ctFile << "# COUNTER_METADATA_BEGIN\n";
  ctFile << "# {\n";

  // Device-wide counter list (same fields as AieProfileCTWriter::writeCTFile begin block)
  ctFile << "#   \"counter_metadata\": [\n";
  for (size_t i = 0; i < allCounters.size(); i++) {
    const auto& counter = allCounters[i];
    ctFile << "#     {\"column\": " << static_cast<int>(counter.column)
           << ", \"row\": " << static_cast<int>(counter.row)
           << ", \"counter\": " << static_cast<int>(counter.counterNumber)
           << ", \"module\": \"" << counter.module
           << "\", \"address\": \"" << formatAddress(counter.address) << "\"";
    if (!counter.metricSet.empty()) {
      ctFile << ", \"metric_set\": \"" << counter.metricSet << "\"";
    }
    if (!counter.portDirection.empty()) {
      ctFile << ", \"port_direction\": \"" << counter.portDirection << "\"";
    }
    ctFile << "}";
    if (i < allCounters.size() - 1)
      ctFile << ",";
    ctFile << "\n";
  }
  ctFile << "#   ],\n";

  // Collect ASM groups that have counters
  std::vector<const ASMFileInfo*> metaGroups;
  for (const auto& asmFile : asmFiles) {
    if (!asmFile.counters.empty())
      metaGroups.push_back(&asmFile);
  }

  for (size_t g = 0; g < metaGroups.size(); g++) {
    const auto& asmFile = *metaGroups[g];
    ctFile << "#   \"" << asmFile.asmId << "\": [\n";

    for (size_t c = 0; c < asmFile.counters.size(); c++) {
      const auto& ctr = asmFile.counters[c];
      ctFile << "#     {\"col\": " << static_cast<int>(ctr.column)
             << ", \"row\": " << static_cast<int>(ctr.row)
             << ", \"ctr\": " << static_cast<int>(ctr.counterNumber)
             << ", \"module\": \"" << ctr.module << "\""
             << ", \"dir\": ";

      if (ctr.portDirection == "input")
        ctFile << "\"i\"";
      else if (ctr.portDirection == "output")
        ctFile << "\"o\"";
      else
        ctFile << "null";

      ctFile << "}";
      if (c < asmFile.counters.size() - 1)
        ctFile << ",";
      ctFile << "\n";
    }

    ctFile << "#   ]";
    if (g < metaGroups.size() - 1)
      ctFile << ",";
    ctFile << "\n";
  }

  ctFile << "# }\n";
  ctFile << "# COUNTER_METADATA_END\n";
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
    ctFile << "    ts_" << asmFile.asmId << " = timestamp32()\n";

    // Write counter reads using _ as throwaway variable
    for (size_t i = 0; i < asmFile.counters.size(); i++) {
      ctFile << "    _ = read_reg("
             << formatAddress(asmFile.counters[i].address) << ")\n";
    }

    ctFile << "}\n\n";
  }

  // Write end block
  ctFile << "end\n";
  ctFile << "{\n";
  ctFile << "    ts_end = timestamp32()\n";
  ctFile << "}\n";

  ctFile.close();

  std::stringstream msg;
  msg << "Generated CT file with embedded counter metadata: " << outputPath;
  xrt_core::message::send(severity_level::info, "XRT", msg.str());

  return true;
}

} // namespace xdp

