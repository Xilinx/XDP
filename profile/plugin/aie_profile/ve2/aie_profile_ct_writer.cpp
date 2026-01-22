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
#include <regex>
#include <sstream>

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
  // Step 1: Find all aie_runtime_control<id>.asm files
  auto asmFiles = findASMFiles();
  if (asmFiles.empty()) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "No aie_runtime_control<id>.asm files found. CT file will not be generated.");
    return false;
  }

  // Step 2: Get all configured counters
  auto allCounters = getConfiguredCounters();
  if (allCounters.empty()) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "No AIE counters configured. CT file will not be generated.");
    return false;
  }

  // Step 3: Parse SAVE_TIMESTAMPS and filter counters for each ASM file
  bool hasTimestamps = false;
  for (auto& asmFile : asmFiles) {
    // Parse SAVE_TIMESTAMPS from ASM file
    asmFile.timestamps = parseSaveTimestamps(asmFile.filename);
    if (!asmFile.timestamps.empty())
      hasTimestamps = true;

    // Filter counters for this ASM file's column range
    asmFile.counters = filterCountersByColumn(allCounters, 
                                               asmFile.colStart, 
                                               asmFile.colEnd);
  }

  if (!hasTimestamps) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "No SAVE_TIMESTAMPS instructions found in ASM files. CT file will not be generated.");
    return false;
  }

  // Step 4: Generate the CT file
  return writeCTFile(asmFiles, allCounters);
}

std::vector<ASMFileInfo> AieProfileCTWriter::findASMFiles()
{
  std::vector<ASMFileInfo> asmFiles;
  
  // Regex pattern to match aie_runtime_control<id>.asm
  std::regex filenamePattern(R"(aie_runtime_control(\d+)\.asm)");
  std::smatch match;

  try {
    // Search recursively in current working directory and all subdirectories
    for (const auto& entry : fs::recursive_directory_iterator(fs::current_path())) {
      if (!entry.is_regular_file())
        continue;

      std::string filename = entry.path().filename().string();
      if (std::regex_match(filename, match, filenamePattern)) {
        ASMFileInfo info;
        info.filename = entry.path().string();
        info.asmId = std::stoi(match[1].str());
        info.ucNumber = 4 * info.asmId;
        info.colStart = info.asmId * 4;
        info.colEnd = info.colStart + 3;

        asmFiles.push_back(info);

        std::stringstream msg;
        msg << "Found ASM file: " << info.filename << " (id=" << info.asmId 
            << ", uc=" << info.ucNumber << ", columns " << info.colStart 
            << "-" << info.colEnd << ")";
        xrt_core::message::send(severity_level::debug, "XRT", msg.str());
      }
    }
  }
  catch (const std::exception& e) {
    std::stringstream msg;
    msg << "Error searching for ASM files: " << e.what();
    xrt_core::message::send(severity_level::warning, "XRT", msg.str());
  }

  // Sort by ASM ID for consistent output
  std::sort(asmFiles.begin(), asmFiles.end(), 
            [](const ASMFileInfo& a, const ASMFileInfo& b) {
              return a.asmId < b.asmId;
            });

  return asmFiles;
}

std::vector<SaveTimestampInfo> AieProfileCTWriter::parseSaveTimestamps(const std::string& filepath)
{
  std::vector<SaveTimestampInfo> timestamps;

  std::ifstream file(filepath);
  if (!file.is_open()) {
    std::stringstream msg;
    msg << "Unable to open ASM file: " << filepath;
    xrt_core::message::send(severity_level::warning, "XRT", msg.str());
    return timestamps;
  }

  // Regex pattern to match SAVE_TIMESTAMPS with optional index
  // Matches: "SAVE_TIMESTAMPS" or "SAVE_TIMESTAMPS <number>"
  std::regex timestampPattern(R"(\s*SAVE_TIMESTAMPS\s*(\d*))", std::regex::icase);
  std::smatch match;

  std::string line;
  uint32_t lineNumber = 0;

  while (std::getline(file, line)) {
    lineNumber++;

    if (std::regex_search(line, match, timestampPattern)) {
      SaveTimestampInfo info;
      info.lineNumber = lineNumber;
      
      // Check if an optional index was provided
      if (match[1].matched && !match[1].str().empty()) {
        info.optionalIndex = std::stoi(match[1].str());
      } else {
        info.optionalIndex = -1;
      }

      timestamps.push_back(info);
    }
  }

  std::stringstream msg;
  msg << "Found " << timestamps.size() << " SAVE_TIMESTAMPS in " << filepath;
  xrt_core::message::send(severity_level::debug, "XRT", msg.str());

  return timestamps;
}

std::vector<CTCounterInfo> AieProfileCTWriter::getConfiguredCounters()
{
  std::vector<CTCounterInfo> counters;

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
           << "\", \"address\": \"" << formatAddress(counter.address) << "\"}";
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

    ctFile << "    print(f\"Probe fired: ts={ts}\")\n";
    ctFile << "@blockopen\n";
    ctFile << "profile_data[\"probes\"].append({\n";
    ctFile << "    \"asm_file\": \"" << basename << "\",\n";
    ctFile << "    \"timestamp\": ts,\n";
    ctFile << "    \"counters\": [";

    // Write counter variable names
    for (size_t i = 0; i < asmFile.counters.size(); i++) {
      if (i > 0)
        ctFile << ", ";
      ctFile << "ctr_" << i;
    }

    ctFile << "]\n";
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

