// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved

#ifndef AIE_DTRACE_CT_WRITER_H
#define AIE_DTRACE_CT_WRITER_H

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "aiebu/aiebu_assembler.h"

namespace xdp {

// Forward declarations
class VPDatabase;
class AieProfileMetadata;
struct AIECounter;

/**
 * @brief Information about a SAVE_TIMESTAMPS instruction found in ASM files
 */
struct SaveTimestampInfo {
  uint32_t lineNumber;
  int optionalIndex;  // -1 if no index specified
};

/**
 * @brief Information about a counter for the CT file
 */
struct CTCounterInfo {
  uint8_t column;
  uint8_t row;
  uint8_t counterNumber;
  std::string module;
  uint64_t address;
  std::string metricSet;      // Metric set name for this counter
  std::string portDirection;  // "input"/"output" for throughput metrics (empty otherwise)
};

/**
 * @brief Information about an ASM file and its associated counters
 */
struct ASMFileInfo {
  std::string filename;
  int asmId;                                    // Extracted from aie_runtime_control<id>.asm
  int ucNumber;                                 // UC start column (jprobe :ucN); from aiebu op_loc or asmId*4 (CSV)
  int colStart;                                 // Counter filter range start; from aiebu or asmId*4 (CSV)
  int colEnd;                                   // Inclusive end; op_loc: next UC start-1, else max(opLocMaxCol, max counter col); CSV: asmId-based + last UC extended
  /// Min/max AIE column from aiebu .dump (op_loc lineinfo.col); UINT32_MAX when built from CSV only
  uint32_t opLocMinCol = UINT32_MAX;
  uint32_t opLocMaxCol = 0;
  std::vector<SaveTimestampInfo> timestamps;   // SAVE_TIMESTAMPS lines
  std::vector<CTCounterInfo> counters;         // Filtered counters for this ASM
};

/**
 * @class AieDtraceCTWriter
 * @brief Generates CT (CERT Tracing) files for VE2 AIE profiling
 *
 * This class searches for aie_runtime_control<id>.asm files in the current
 * working directory, parses SAVE_TIMESTAMPS instructions, retrieves configured
 * AIE counters, and generates a CT file that can capture performance counter
 * data at each SAVE_TIMESTAMPS instruction.
 */
class AieDtraceCTWriter {
public:
  /**
   * @brief Constructor
   * @param database Pointer to the VPDatabase for accessing counter configuration
   * @param metadata Pointer to AieProfileMetadata for AIE configuration info
   * @param deviceId The device ID for which to generate the CT file
   * @param startCol Absolute start column of the hw_context partition; added to
   *                 relative counter columns so the CT file contains absolute
   *                 hardware addresses regardless of where XRT placed the partition
   */
  AieDtraceCTWriter(VPDatabase* database,
                     std::shared_ptr<AieProfileMetadata> metadata,
                     uint64_t deviceId,
                     uint8_t startCol);

  /**
   * @brief Destructor
   */
  ~AieDtraceCTWriter() = default;

  /**
   * @brief Generate the CT file using the default output path
   * @return true if CT file was generated successfully, false otherwise
   */
  bool generate();

  /**
   * @brief Generate the CT file at a caller-specified path
   * @param outputPath Full path for the generated CT file
   * @return true if CT file was generated successfully, false otherwise
   */
  bool generate(const std::string& outputPath);

  /**
   * @brief Generate the CT file using op_loc data from aiebu_assembler
   * @param outputPath Full path for the generated CT file
   * @param opLocations Vector of op_loc from aiebu_assembler::get_op_locations
   * @return true if CT file was generated successfully, false otherwise
   */
  bool generate(const std::string& outputPath,
                const std::vector<aiebu::aiebu_assembler::op_loc>& opLocations);

private:
  /**
   * @brief Read ASM file information from CSV file
   * @param csvPath Path to the CSV file (aie_profile_timestamps.csv)
   * @return Vector of ASMFileInfo structures with timestamps
   */
  std::vector<ASMFileInfo> readASMInfoFromCSV(const std::string& csvPath);

  /**
   * @brief Get all configured AIE counters from the database
   * @return Vector of CTCounterInfo for all counters
   */
  std::vector<CTCounterInfo> getConfiguredCounters();

  /**
   * @brief Filter counters by column range for a specific ASM file
   * @param allCounters All available counters
   * @param colStart Starting column (inclusive)
   * @param colEnd Ending column (inclusive)
   * @return Vector of CTCounterInfo within the column range
   */
  std::vector<CTCounterInfo> filterCountersByColumn(
      const std::vector<CTCounterInfo>& allCounters,
      int colStart, int colEnd);

  /**
   * @brief Calculate the register address for a counter
   * @param column Tile column
   * @param row Tile row
   * @param counterNumber Counter number within the tile
   * @param module Module type string ("aie", "aie_memory", "interface_tile", "memory_tile")
   * @return 64-bit register address
   */
  uint64_t calculateCounterAddress(uint8_t column, uint8_t row,
                                   uint8_t counterNumber,
                                   const std::string& module);

  /**
   * @brief Write the CT file content
   * @param asmFiles Vector of ASMFileInfo with all parsed information
   * @param allCounters Vector of all CTCounterInfo for metadata
   * @param outputPath Full path for the output CT file
   * @return true if file was written successfully
   */
  bool writeCTFile(const std::vector<ASMFileInfo>& asmFiles,
                   const std::vector<CTCounterInfo>& allCounters,
                   const std::string& outputPath);

  /**
   * @brief Format an address as a hex string
   * @param address The address to format
   * @return Formatted hex string (e.g., "0x0000037520")
   */
  std::string formatAddress(uint64_t address);

  /**
   * @brief Get base offset for a module type
   * @param module Module type string
   * @return Base offset for the module
   */
  uint64_t getModuleBaseOffset(const std::string& module);

  /**
   * @brief Check if metric set is a throughput metric
   * @param metricSet The metric set name
   * @return true if it's a throughput metric
   */
  bool isThroughputMetric(const std::string& metricSet);

  /**
   * @brief Get port direction for a throughput metric
   * @param metricSet The metric set name
   * @param payload The counter payload (encodes master/slave info)
   * @return "input" or "output" for throughput metrics, empty string otherwise
   */
  std::string getPortDirection(const std::string& metricSet, uint64_t payload);

private:
  VPDatabase* db;
  std::shared_ptr<AieProfileMetadata> metadata;
  uint64_t deviceId;

  // AIE configuration values
  uint8_t columnShift;
  uint8_t rowShift;
  uint8_t partitionStartCol;  // Absolute start column of the hw_context partition

  // Base offsets by module type
  static constexpr uint64_t CORE_MODULE_BASE_OFFSET   = 0x00037520;
  static constexpr uint64_t MEMORY_MODULE_BASE_OFFSET = 0x00011020;
  static constexpr uint64_t MEM_TILE_BASE_OFFSET      = 0x00091020;
  static constexpr uint64_t SHIM_TILE_BASE_OFFSET     = 0x00031020;

  // Output filename
  static constexpr const char* CT_OUTPUT_FILENAME = "aie_profile.ct";
};

} // namespace xdp

#endif // AIE_DTRACE_CT_WRITER_H

