// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/ve2/aie_dtrace_ct_writer.h"
#include "xdp/profile/plugin/aie_profile/aie_profile_metadata.h"
#include "xdp/profile/database/database.h"
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/database/static_info/aie_util.h"

#include "core/common/message.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <regex>
#include <sstream>
#include <vector>

#include <boost/property_tree/ptree.hpp>

namespace xdp {

namespace {

// Order UCs by aiebu min column; each UC's width is [colStart, nextUcStart - 1] (last UC ends at opLocMaxCol).
void
applyUcSpansFromOpLoc(std::vector<ASMFileInfo>& asmFileInfoList)
{
  if (asmFileInfoList.empty())
    return;

  std::sort(asmFileInfoList.begin(), asmFileInfoList.end(),
            [](const ASMFileInfo& a, const ASMFileInfo& b) {
              if (a.opLocMinCol != b.opLocMinCol)
                return a.opLocMinCol < b.opLocMinCol;
              return a.filename < b.filename;
            });

  const size_t n = asmFileInfoList.size();
  for (size_t i = 0; i < n; ++i) {
    auto& af = asmFileInfoList[i];
    af.colStart = static_cast<int>(af.opLocMinCol);
    af.ucNumber = af.colStart;
    if (i + 1 < n) {
      const int nextStart = static_cast<int>(asmFileInfoList[i + 1].opLocMinCol);
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
extendLastUcToMaxConfiguredColumn(std::vector<ASMFileInfo>& asmFileInfoList,
                                  const std::vector<CTCounterInfo>& allCounters)
{
  if (asmFileInfoList.empty() || allCounters.empty())
    return;

  int maxCfgCol = -1;
  for (const auto& c : allCounters)
    maxCfgCol = std::max(maxCfgCol, static_cast<int>(c.column));
  if (maxCfgCol < 0)
    return;

  auto& last = asmFileInfoList.back();
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
  std::vector<ASMFileInfo> asmFileInfoList;
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
      auto it = std::find_if(asmFileInfoList.begin(), asmFileInfoList.end(),
          [&fname](const ASMFileInfo& a) { return a.filename == fname; });

      if (it == asmFileInfoList.end()) {
        ASMFileInfo info;
        info.filename = fname;
        info.asmId = match[1].matched ? std::stoi(match[1].str()) : 0;
        info.opLocMinCol = li.col;
        info.opLocMaxCol = li.col;
        asmFileInfoList.push_back(info);
        it = asmFileInfoList.end() - 1;
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

  if (asmFileInfoList.empty())
    return false;

  applyUcSpansFromOpLoc(asmFileInfoList);

  auto allCounters = getConfiguredCounters();
  if (allCounters.empty())
    return false;

  extendLastUcToMaxConfiguredColumn(asmFileInfoList, allCounters);

  for (auto& asmFileInfo : asmFileInfoList) {
    asmFileInfo.counters = filterCountersByColumn(allCounters,
                                               asmFileInfo.colStart, asmFileInfo.colEnd);
  }

  return writeCTFile(asmFileInfoList, allCounters, outputPath);
}

bool AieDtraceCTWriter::generate(const std::string& outputPath)
{
  std::string csvPath = (fs::current_path() / "aie_profile_timestamps.csv").string();
  auto asmFileInfoList = readASMInfoFromCSV(csvPath);
  if (asmFileInfoList.empty()) {
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

  extendLastUcToMaxConfiguredColumn(asmFileInfoList, allCounters);

  bool hasTimestamps = false;
  for (auto& asmFileInfo : asmFileInfoList) {
    if (!asmFileInfo.timestamps.empty())
      hasTimestamps = true;

    asmFileInfo.counters = filterCountersByColumn(allCounters, 
                                               asmFileInfo.colStart, 
                                               asmFileInfo.colEnd);
  }

  if (!hasTimestamps) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "No SAVE_TIMESTAMPS instructions found in CSV. CT file will not be generated.");
    return false;
  }

  return writeCTFile(asmFileInfoList, allCounters, outputPath);
}

std::vector<ASMFileInfo> AieDtraceCTWriter::readASMInfoFromCSV(const std::string& csvPath)
{
  std::vector<ASMFileInfo> asmFileInfoList;

  std::ifstream csvFile(csvPath);
  if (!csvFile.is_open()) {
    std::stringstream msg;
    msg << "Unable to open CSV file: " << csvPath << ". Please run parse_aie_runtime_to_csv.py first.";
    xrt_core::message::send(severity_level::warning, "XRT", msg.str());
    return asmFileInfoList;
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

      asmFileInfoList.push_back(info);

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
  std::sort(asmFileInfoList.begin(), asmFileInfoList.end(), 
            [](const ASMFileInfo& a, const ASMFileInfo& b) {
              if (a.colStart != b.colStart)
                return a.colStart < b.colStart;
              return a.filename < b.filename;
            });

  std::stringstream msg;
  msg << "Loaded " << asmFileInfoList.size() << " ASM files from CSV with "
      << std::accumulate(asmFileInfoList.begin(), asmFileInfoList.end(), 0,
                        [](int sum, const ASMFileInfo& info) { 
                          return sum + info.timestamps.size(); 
                        })
      << " total SAVE_TIMESTAMPS";
  xrt_core::message::send(severity_level::info, "XRT", msg.str());

  return asmFileInfoList;
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

bool AieDtraceCTWriter::writeCTFile(const std::vector<ASMFileInfo>& asmFileInfoList,
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
  for (const auto& asmFileInfo : asmFileInfoList) {
    if (!asmFileInfo.counters.empty())
      metaGroups.push_back(&asmFileInfo);
  }

  for (size_t g = 0; g < metaGroups.size(); g++) {
    const auto& asmFileInfo = *metaGroups[g];
    ctFile << "#   \"" << asmFileInfo.asmId << "\": [\n";

    for (size_t c = 0; c < asmFileInfo.counters.size(); c++) {
      const auto& ctr = asmFileInfo.counters[c];
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
      if (c < asmFileInfo.counters.size() - 1)
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
  for (const auto& asmFileInfo : asmFileInfoList) {
    if (asmFileInfo.timestamps.empty() || asmFileInfo.counters.empty())
      continue;

    std::string basename = fs::path(asmFileInfo.filename).filename().string();

    // Write comment
    ctFile << "# Probes for " << basename 
           << " (columns " << asmFileInfo.colStart << "-" << asmFileInfo.colEnd << ")\n";

    // Build line number list for jprobe
    std::stringstream lineList;
    lineList << "line";
    for (size_t i = 0; i < asmFileInfo.timestamps.size(); i++) {
      if (i > 0)
        lineList << ",";
      lineList << asmFileInfo.timestamps[i].lineNumber;
    }

    // Write jprobe declaration
    ctFile << "jprobe:" << basename 
           << ":uc" << asmFileInfo.ucNumber 
           << ":" << lineList.str() << "\n";
    ctFile << "{\n";
    ctFile << "    ts_" << asmFileInfo.asmId << " = timestamp32()\n";

    // Write counter reads using _ as throwaway variable
    for (size_t i = 0; i < asmFileInfo.counters.size(); i++) {
      ctFile << "    _ = read_reg("
             << formatAddress(asmFileInfo.counters[i].address) << ")\n";
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

std::vector<uint8_t> AieDtraceCTWriter::getShimTileColumns(void* hwctx)
{
  std::vector<uint8_t> columns;
  
  if (!hwctx) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "AIE dtrace: No hwctx provided for shim column discovery");
    return columns;
  }

  try {
    boost::property_tree::ptree aiePartitionPt = xdp::aie::getAIEPartitionInfo(hwctx);
    if (aiePartitionPt.empty()) {
      xrt_core::message::send(severity_level::debug, "XRT",
          "AIE dtrace: No partition info available");
      return columns;
    }

    uint8_t numCols = static_cast<uint8_t>(aiePartitionPt.back().second.get<uint64_t>("num_cols"));

    // Return relative columns (0, 1, 2, ...) for hardware configuration
    for (uint8_t i = 0; i < numCols; ++i) {
      columns.push_back(i);
    }

    std::stringstream msg;
    msg << "AIE dtrace: Found " << static_cast<int>(numCols) << " shim columns (relative: 0-"
        << static_cast<int>(numCols - 1) << ")";
    xrt_core::message::send(severity_level::debug, "XRT", msg.str());
  }
  catch (const std::exception& e) {
    std::stringstream msg;
    msg << "AIE dtrace: Error getting shim columns: " << e.what();
    xrt_core::message::send(severity_level::warning, "XRT", msg.str());
  }

  return columns;
}

std::vector<BandwidthCounterConfig> AieDtraceCTWriter::getBandwidthCounterConfigs()
{
  // VE2 shim tile DMA port indices for stream switch event monitoring
  // These port indices are architecture-specific and map to the physical
  // stream switch ports that connect to the DMA channels.
  //
  // For VE2 shim tiles:
  // - S2MM (master) ports: Stream switch master port feeds data to DMA S2MM
  // - MM2S (slave) ports: Stream switch slave port receives data from DMA MM2S
  //
  // Port encoding in Stream_Switch_Event_Port_Selection register:
  // - Bits [4:0]: Port index
  // - Bit [5]: 0 = slave, 1 = master
  //
  // VE2 shim tile port mapping:
  // - S2MM ch0: master South1 => port index 3
  // - S2MM ch1: master South3 => port index 5
  // - MM2S ch0: slave South3  => port index 5
  // - MM2S ch1: slave South7  => port index 9
  //
  // counterNumber, channel, dmaPortIndex, isMaster, direction
  return {
    {0, 0, 3, true,  "input"},   // Counter 0: S2MM Ch0 (master South1) - read_bandwidth
    {1, 1, 5, true,  "input"},   // Counter 1: S2MM Ch1 (master South3) - read_bandwidth
    {2, 0, 5, false, "output"},  // Counter 2: MM2S Ch0 (slave South3) - write_bandwidth
    {3, 1, 9, false, "output"}   // Counter 3: MM2S Ch1 (slave South7) - write_bandwidth
  };
}

std::vector<CTRegisterWrite> AieDtraceCTWriter::generateStreamSwitchPortConfig(uint8_t column)
{
  std::vector<CTRegisterWrite> writes;

  uint64_t tileAddress = (static_cast<uint64_t>(column) << columnShift) |
                         (static_cast<uint64_t>(SHIM_ROW) << rowShift);
  uint64_t regAddr = tileAddress + STREAM_SWITCH_EVENT_PORT_SEL_OFFSET;

  auto configs = getBandwidthCounterConfigs();

  // Build the register value: 4 ports packed into 32 bits, 8 bits per port
  // Each port: bits [4:0] = DMA port index, bit [5] = slave(0)/master(1)
  uint32_t regValue = 0;
  for (size_t i = 0; i < configs.size() && i < PORTS_PER_REGISTER; ++i) {
    const auto& cfg = configs[i];
    uint8_t slaveOrMaster = cfg.isMaster ? 1 : 0;
    uint8_t bitOffset = static_cast<uint8_t>(i) * 8;
    regValue |= (static_cast<uint32_t>(cfg.dmaPortIndex) << bitOffset)
              | (static_cast<uint32_t>(slaveOrMaster) << (bitOffset + 5));
  }

  std::stringstream comment;
  comment << "SS port sel @ col " << static_cast<int>(column)
          << " (S2MM ch0,ch1; MM2S ch0,ch1)";

  CTRegisterWrite write;
  write.address = regAddr;
  write.value = regValue;
  write.comment = comment.str();
  writes.push_back(write);

  return writes;
}

std::vector<CTRegisterWrite> AieDtraceCTWriter::generatePerfCounterConfig(uint8_t column)
{
  std::vector<CTRegisterWrite> writes;

  uint64_t tileAddress = (static_cast<uint64_t>(column) << columnShift) |
                         (static_cast<uint64_t>(SHIM_ROW) << rowShift);

  // Performance counter register addresses (aie2ps_pl_module):
  // Performance_Counter0-3: 0x00031020, 0x00031024, 0x00031028, 0x0003102C
  constexpr uint64_t PERF_COUNTER0_OFFSET = 0x00031020;

  // Reset performance counters 0-3 to zero
  for (uint8_t i = 0; i < 4; ++i) {
    CTRegisterWrite write;
    write.address = tileAddress + PERF_COUNTER0_OFFSET + (i * 4);
    write.value = 0;
    write.comment = "Reset PerfCounter" + std::to_string(i) + " @ col " + std::to_string(column);
    writes.push_back(write);
  }

  // Performance control register addresses (aie2ps_pl_module):
  // Performance_Ctrl0: 0x00031000 - Counters 0,1 start/stop events
  // Performance_Ctrl2: 0x0003100C - Counters 2,3 start/stop events
  constexpr uint64_t PERF_CTRL0_OFFSET = 0x00031000;
  constexpr uint64_t PERF_CTRL2_OFFSET = 0x0003100C;

  // PORT_RUNNING events for byte counting (aie2ps shim tile events)
  // Port_Running_N events: 134, 138, 142, 146 (decimal)
  constexpr uint8_t PORT_RUNNING_0_PL_EVENT = 0x86;  // 134
  constexpr uint8_t PORT_RUNNING_1_PL_EVENT = 0x8A;  // 138
  constexpr uint8_t PORT_RUNNING_2_PL_EVENT = 0x8E;  // 142
  constexpr uint8_t PORT_RUNNING_3_PL_EVENT = 0x92;  // 146

  uint8_t startEvents[4] = {
    PORT_RUNNING_0_PL_EVENT,
    PORT_RUNNING_1_PL_EVENT,
    PORT_RUNNING_2_PL_EVENT,
    PORT_RUNNING_3_PL_EVENT
  };

  // Performance_Ctrl0: counters 0 and 1
  // Bit layout: [31:24]=Cnt1_Stop, [23:16]=Cnt1_Start, [15:8]=Cnt0_Stop, [7:0]=Cnt0_Start
  {
    uint32_t regValue = 0;
    regValue |= (static_cast<uint32_t>(startEvents[0]) & 0xFF) << 0;   // Cnt0_Start_Event
    regValue |= (static_cast<uint32_t>(startEvents[0]) & 0xFF) << 8;   // Cnt0_Stop_Event
    regValue |= (static_cast<uint32_t>(startEvents[1]) & 0xFF) << 16;  // Cnt1_Start_Event
    regValue |= (static_cast<uint32_t>(startEvents[1]) & 0xFF) << 24;  // Cnt1_Stop_Event

    CTRegisterWrite write;
    write.address = tileAddress + PERF_CTRL0_OFFSET;
    write.value = regValue;
    write.comment = "PerfCtrl0 @ col " + std::to_string(column) + " (ctr0,ctr1)";
    writes.push_back(write);
  }

  // Performance_Ctrl2: counters 2 and 3
  // Bit layout: [31:24]=Cnt3_Stop, [23:16]=Cnt3_Start, [15:8]=Cnt2_Stop, [7:0]=Cnt2_Start
  {
    uint32_t regValue = 0;
    regValue |= (static_cast<uint32_t>(startEvents[2]) & 0xFF) << 0;   // Cnt2_Start_Event
    regValue |= (static_cast<uint32_t>(startEvents[2]) & 0xFF) << 8;   // Cnt2_Stop_Event
    regValue |= (static_cast<uint32_t>(startEvents[3]) & 0xFF) << 16;  // Cnt3_Start_Event
    regValue |= (static_cast<uint32_t>(startEvents[3]) & 0xFF) << 24;  // Cnt3_Stop_Event

    CTRegisterWrite write;
    write.address = tileAddress + PERF_CTRL2_OFFSET;
    write.value = regValue;
    write.comment = "PerfCtrl2 @ col " + std::to_string(column) + " (ctr2,ctr3)";
    writes.push_back(write);
  }

  return writes;
}

std::vector<CTCounterInfo> AieDtraceCTWriter::generateBandwidthCounters(
    const std::vector<uint8_t>& shimColumns)
{
  std::vector<CTCounterInfo> counters;
  auto configs = getBandwidthCounterConfigs();

  for (uint8_t column : shimColumns) {
    for (const auto& cfg : configs) {
      CTCounterInfo info;
      info.column = column;
      info.row = SHIM_ROW;
      info.counterNumber = cfg.counterNumber;
      info.module = "interface_tile";
      info.address = calculateCounterAddress(column, SHIM_ROW, cfg.counterNumber, "interface_tile");
      info.metricSet = "ddr_bandwidth";
      info.portDirection = cfg.direction;
      counters.push_back(info);
    }
  }

  return counters;
}

bool AieDtraceCTWriter::writeBandwidthCTFile(
    const std::vector<ASMFileInfo>& asmFileInfoList,
    const std::vector<CTCounterInfo>& allCounters,
    const std::vector<CTRegisterWrite>& beginBlockWrites,
    const std::string& outputPath)
{
  std::ofstream ctFile(outputPath);

  if (!ctFile.is_open()) {
    std::stringstream msg;
    msg << "Unable to create CT file: " << outputPath;
    xrt_core::message::send(severity_level::warning, "XRT", msg.str());
    return false;
  }

  ctFile << "# Auto-generated CT file for AIE bandwidth monitoring\n";
  ctFile << "# Generated by XRT AIE Dtrace Plugin (simplified bandwidth mode)\n";
  ctFile << "# Fixed 4 counters per shim tile: S2MM ch0,ch1 + MM2S ch0,ch1\n";
  ctFile << "# Post-processing filters by metric: read_bandwidth, write_bandwidth, ddr_bandwidth\n\n";

  ctFile << "begin\n";
  ctFile << "{\n";
  ctFile << "    ts_start = timestamp32()\n";

  if (!beginBlockWrites.empty()) {
    ctFile << "\n    # Hardware configuration for bandwidth counters\n";
    for (const auto& write : beginBlockWrites) {
      if (!write.comment.empty())
        ctFile << "    # " << write.comment << "\n";
      ctFile << "    write_reg(" << formatAddress(write.address)
             << ", 0x" << std::hex << std::setfill('0') << std::setw(8)
             << write.value << std::dec << ")\n";
    }
    ctFile << "\n";
  }

  ctFile << "@blockopen\n";
  ctFile << "# COUNTER_METADATA_BEGIN\n";
  ctFile << "# {\n";

  // Per-UC counter metadata groupings only
  std::vector<const ASMFileInfo*> metaGroups;
  for (const auto& asmFileInfo : asmFileInfoList) {
    if (!asmFileInfo.counters.empty())
      metaGroups.push_back(&asmFileInfo);
  }

  for (size_t g = 0; g < metaGroups.size(); g++) {
    const auto& asmFileInfo = *metaGroups[g];
    ctFile << "#   \"" << asmFileInfo.asmId << "\": [\n";

    for (size_t c = 0; c < asmFileInfo.counters.size(); c++) {
      const auto& ctr = asmFileInfo.counters[c];
      uint8_t channel = ctr.counterNumber % 2;
      ctFile << "#     {\"col\": " << static_cast<int>(ctr.column)
             << ", \"row\": " << static_cast<int>(ctr.row)
             << ", \"ctr\": " << static_cast<int>(ctr.counterNumber)
             << ", \"ch\": " << static_cast<int>(channel)
             << ", \"dir\": ";

      if (ctr.portDirection == "input")
        ctFile << "\"i\"";
      else if (ctr.portDirection == "output")
        ctFile << "\"o\"";
      else
        ctFile << "null";

      ctFile << "}";
      if (c < asmFileInfo.counters.size() - 1)
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

  for (const auto& asmFileInfo : asmFileInfoList) {
    if (asmFileInfo.timestamps.empty() || asmFileInfo.counters.empty())
      continue;

    std::string basename = fs::path(asmFileInfo.filename).filename().string();

    ctFile << "# Probes for " << basename
           << " (columns " << asmFileInfo.colStart << "-" << asmFileInfo.colEnd << ")\n";

    std::stringstream lineList;
    lineList << "line";
    for (size_t i = 0; i < asmFileInfo.timestamps.size(); i++) {
      if (i > 0)
        lineList << ",";
      lineList << asmFileInfo.timestamps[i].lineNumber;
    }

    ctFile << "jprobe:" << basename
           << ":uc" << asmFileInfo.ucNumber
           << ":" << lineList.str() << "\n";
    ctFile << "{\n";
    ctFile << "    ts_" << asmFileInfo.asmId << " = timestamp32()\n";

    for (size_t i = 0; i < asmFileInfo.counters.size(); i++) {
      const auto& ctr = asmFileInfo.counters[i];
      ctFile << "    _ = read_reg(" << formatAddress(ctr.address) << ")\n";
    }

    ctFile << "}\n\n";
  }

  ctFile << "end\n";
  ctFile << "{\n";
  ctFile << "    ts_end = timestamp32()\n";
  ctFile << "}\n";

  ctFile.close();

  std::stringstream msg;
  msg << "Generated bandwidth CT file: " << outputPath
      << " (" << allCounters.size() << " counters)";
  xrt_core::message::send(severity_level::info, "XRT", msg.str());

  return true;
}

bool AieDtraceCTWriter::generateBandwidthCT(
    const std::string& outputPath,
    void* hwctx,
    const std::vector<aiebu::aiebu_assembler::op_loc>& opLocations)
{
  if (opLocations.empty()) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "AIE dtrace: No op_locations provided for bandwidth CT generation");
    return false;
  }

  auto shimColumns = getShimTileColumns(hwctx);
  if (shimColumns.empty()) {
    xrt_core::message::send(severity_level::warning, "XRT",
        "AIE dtrace: No shim columns found in partition. Cannot generate bandwidth CT.");
    return false;
  }

  std::vector<ASMFileInfo> asmFileInfoList;
  std::regex filenamePattern(R"(aie_runtime_control(\d+)?\.asm)");

  for (const auto& loc : opLocations) {
    for (const auto& li : loc.line_info) {
      if (li.entries.empty())
        continue;

      const auto& fname = li.entries.front().second;
      std::smatch match;
      if (!std::regex_search(fname, match, filenamePattern))
        continue;

      auto it = std::find_if(asmFileInfoList.begin(), asmFileInfoList.end(),
          [&fname](const ASMFileInfo& a) { return a.filename == fname; });

      if (it == asmFileInfoList.end()) {
        ASMFileInfo info;
        info.filename = fname;
        info.asmId = match[1].matched ? std::stoi(match[1].str()) : 0;
        info.opLocMinCol = li.col;
        info.opLocMaxCol = li.col;
        asmFileInfoList.push_back(info);
        it = asmFileInfoList.end() - 1;
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

  if (asmFileInfoList.empty()) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "AIE dtrace: No ASM files found in op_locations for bandwidth CT generation");
    return false;
  }

  applyUcSpansFromOpLoc(asmFileInfoList);

  auto allCounters = generateBandwidthCounters(shimColumns);
  if (allCounters.empty()) {
    xrt_core::message::send(severity_level::warning, "XRT",
        "AIE dtrace: No bandwidth counters generated");
    return false;
  }

  extendLastUcToMaxConfiguredColumn(asmFileInfoList, allCounters);

  for (auto& asmFileInfo : asmFileInfoList) {
    asmFileInfo.counters = filterCountersByColumn(allCounters, asmFileInfo.colStart, asmFileInfo.colEnd);
  }

  std::vector<CTRegisterWrite> beginBlockWrites;
  for (uint8_t column : shimColumns) {
    auto ssWrites = generateStreamSwitchPortConfig(column);
    beginBlockWrites.insert(beginBlockWrites.end(), ssWrites.begin(), ssWrites.end());

    auto pcWrites = generatePerfCounterConfig(column);
    beginBlockWrites.insert(beginBlockWrites.end(), pcWrites.begin(), pcWrites.end());
  }

  return writeBandwidthCTFile(asmFileInfoList, allCounters, beginBlockWrites, outputPath);
}

} // namespace xdp

