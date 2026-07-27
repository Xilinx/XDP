// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

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
class AieDtraceMetadata;
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
  uint8_t channel;            // DMA channel number (0 or 1) for bandwidth metrics
  std::string module;
  uint64_t address;
  std::string metricSet;      // Metric set name for this counter
  std::string portDirection;  // "input"/"output" for throughput metrics (empty otherwise)
  std::string eventType;      // "running"/"stalled" for peak bandwidth metrics (empty otherwise)
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
 * @brief Register write operation for CT file begin block
 */
struct CTRegisterWrite {
  uint64_t address;
  uint32_t value;
  std::string comment;
};

/**
 * @brief Configuration for a single bandwidth counter in a shim tile
 * 
 * Direction is from AIE/application perspective:
 * - "input" = data read FROM DDR into AIE = MM2S channels (Memory-Mapped to Stream)
 * - "output" = data written TO DDR from AIE = S2MM channels (Stream to Memory-Mapped)
 * 
 * For VE2 shim tiles, DMA channels are accessed via stream switch ports:
 * - S2MM (master): Stream switch master port feeds data to DMA (output from AIE)
 * - MM2S (slave): Stream switch slave port receives data from DMA (input to AIE)
 * 
 * The dmaPortIndex is the physical stream switch port index that connects
 * to the DMA channel. This is architecture-specific.
 */
struct BandwidthCounterConfig {
  uint8_t counterNumber;   // Counter number (0-3)
  uint8_t channel;         // DMA channel number (0 or 1)
  uint8_t dmaPortIndex;    // Physical port index for stream switch (VE2-specific)
  bool isMaster;           // true=S2MM/output (master), false=MM2S/input (slave)
  std::string direction;   // "input" (MM2S) or "output" (S2MM)
  std::string eventType;   // "running" or "stalled"
};

/**
 * @brief Resolved configuration for the single core-tile compute_io_bound counter.
 *
 * Two modes, selected by design type:
 * - Reloadable design (useStartStop=false): the compute counter is PC_Range_0-1
 *   over [startPc(=wpc), PROG_MEM_END]. Works because wrapper+kernel are contiguous.
 * - Static/inlined design (useStartStop=true): kernelWrapper is inlined into main's
 *   inner loop; the compute counter is driven by PC breakpoint events - Start=PC_0
 *   at startPc (loop header), Stop=PC_1 at stopPc (loop back-edge) - so it accumulates
 *   the whole loop iteration including the dispatched kernel call.
 *
 * absRow is the ABSOLUTE core-tile row (relative row 0 + aie_tile_row_start) used for
 * register addressing.
 */
struct ComputeIoCoreConfig {
  bool valid = false;
  bool useStartStop = false;  // false = reloadable PC-range, true = static start/stop
  uint32_t startPc = 0;       // wpc (reloadable) or start_pc / loop header (static)
  uint32_t stopPc = 0;        // stop_pc / loop back-edge (static only)
  uint8_t absRow = 0;         // absolute core-tile row for register addressing
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
   * @param metadata Pointer to AieDtraceMetadata for AIE configuration info
   * @param deviceId The device ID for which to generate the CT file
   * @param startCol Absolute start column of the hw_context partition; added to
   *                 relative counter columns so the CT file contains absolute
   *                 hardware addresses regardless of where XRT placed the partition
   */
  AieDtraceCTWriter(VPDatabase* database,
                     std::shared_ptr<AieDtraceMetadata> metadata,
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

  /**
   * @brief Generate a self-contained CT file for bandwidth metrics
   * 
   * This method generates a CT file that configures a fixed set of 4 performance
   * counters and 4 stream switch event ports per shim tile for bandwidth monitoring.
   * It does not depend on setMetricsSettings() - only needs partition info and
   * SAVE_TIMESTAMPS locations.
   * 
   * @param outputPath Full path for the generated CT file
   * @param hwctx Hardware context handle for partition info access
   * @param opLocations Vector of op_loc from aiebu_assembler::get_op_locations
   * @param metricSet The metric set name (ddr_bandwidth, peak_read_bandwidth, etc.)
   * @param channel DMA channel (0 or 1) selected by the metric's channel suffix;
   *                only used by the detailed_ddr_*_bandwidth metric sets
   * @return true if CT file was generated successfully, false otherwise
   */
  bool generateBandwidthCT(const std::string& outputPath,
                           void* hwctx,
                           const std::vector<aiebu::aiebu_assembler::op_loc>& opLocations,
                           const std::string& metricSet = "ddr_bandwidth",
                           uint8_t channel = 0);

  /**
   * @brief Generate a self-contained CT file for the compute_io_bound metric
   *
   * Configures a single core (aie) tile with two core performance counters:
   *   - Counter 2 (kernelWrapper/Compute): reloadable -> PC_Range_0-1 over
   *     [startPc, PROG_MEM_END]; static-inline -> Start=PC_0@startPc, Stop=PC_1@stopPc
   *   - Counter 3 (Total): PC_Range_2-3 over [0, PROG_MEM_END]
   *
   * @param outputPath Full path for the generated CT file
   * @param hwctx Hardware context handle for partition info access
   * @param opLocations Vector of op_loc from aiebu_assembler::get_op_locations
   * @param cfg Resolved compute_io_bound core configuration
   * @return true if CT file was generated successfully, false otherwise
   */
  bool generateComputeIoBoundCT(const std::string& outputPath,
                                void* hwctx,
                                const std::vector<aiebu::aiebu_assembler::op_loc>& opLocations,
                                const ComputeIoCoreConfig& cfg);

  /**
   * @brief Generate a self-contained CT file combining bandwidth and/or
   *        compute_io_bound metrics into a single begin block + counter reads.
   *
   * Either family can be enabled independently; when both are enabled the shim
   * bandwidth counters and the core compute_io_bound counters are emitted into
   * the same CT file.
   *
   * @param outputPath Full path for the generated CT file
   * @param hwctx Hardware context handle for partition info access
   * @param opLocations Vector of op_loc from aiebu_assembler::get_op_locations
   * @param includeBandwidth Emit interface-tile bandwidth counters
   * @param bandwidthMetricSet Bandwidth metric set (used when includeBandwidth)
   * @param bandwidthChannel DMA channel for detailed_ddr_*_bandwidth sets
   * @param includeComputeIoBound Emit the single core-tile compute_io_bound counters
   * @param computeIoCfg Resolved compute_io_bound core configuration (used when includeComputeIoBound)
   * @return true if CT file was generated successfully, false otherwise
   */
  bool generateCT(const std::string& outputPath,
                  void* hwctx,
                  const std::vector<aiebu::aiebu_assembler::op_loc>& opLocations,
                  bool includeBandwidth,
                  const std::string& bandwidthMetricSet,
                  uint8_t bandwidthChannel,
                  bool includeComputeIoBound,
                  const ComputeIoCoreConfig& computeIoCfg);

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
   * @param asmFileInfoList Vector of ASMFileInfo with all parsed information
   * @param allCounters Vector of all CTCounterInfo for metadata
   * @param outputPath Full path for the output CT file
   * @return true if file was written successfully
   */
  bool writeCTFile(const std::vector<ASMFileInfo>& asmFileInfoList,
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

  /**
   * @brief Get shim tile columns from partition info
   * @param hwctx Hardware context handle
   * @return Vector of shim tile column numbers in the partition
   */
  std::vector<uint8_t> getShimTileColumns(void* hwctx);

  /**
   * @brief Generate stream switch port configuration for DMA channels per shim tile
   * @param column Shim tile column
   * @param metricSet The metric set name (ddr_bandwidth, peak_read_bandwidth, etc.)
   * @param channel DMA channel (0 or 1); only used by detailed_ddr_*_bandwidth sets
   * @return Vector of register writes to configure stream switch ports
   */
  std::vector<CTRegisterWrite> generateStreamSwitchPortConfig(uint8_t column,
      const std::string& metricSet = "ddr_bandwidth", uint8_t channel = 0);

  /**
   * @brief Generate performance counter configuration for 4 counters per shim tile
   * @param column Shim tile column
   * @param metricSet The metric set name (ddr_bandwidth, peak_read_bandwidth, etc.)
   * @param channel DMA channel (0 or 1); only used by detailed_ddr_*_bandwidth sets
   * @return Vector of register writes to configure performance counters
   */
  std::vector<CTRegisterWrite> generatePerfCounterConfig(uint8_t column,
      const std::string& metricSet = "ddr_bandwidth", uint8_t channel = 0);

  /**
   * @brief Get bandwidth counter configurations for a shim tile based on metric set
   * @param metricSet The metric set name (ddr_bandwidth, peak_read_bandwidth, etc.)
   * @param channel DMA channel (0 or 1); only used by detailed_ddr_*_bandwidth sets
   * @return Vector of BandwidthCounterConfig for the 4 counters
   */
  std::vector<BandwidthCounterConfig> getBandwidthCounterConfigs(
      const std::string& metricSet = "ddr_bandwidth", uint8_t channel = 0);

  /**
   * @brief Generate bandwidth counters for all shim tiles in the partition
   * @param shimColumns Vector of shim tile columns
   * @param metricSet The metric set name (ddr_bandwidth, peak_read_bandwidth, etc.)
   * @param channel DMA channel (0 or 1); only used by detailed_ddr_*_bandwidth sets
   * @return Vector of CTCounterInfo for all bandwidth counters
   */
  std::vector<CTCounterInfo> generateBandwidthCounters(const std::vector<uint8_t>& shimColumns,
      const std::string& metricSet = "ddr_bandwidth", uint8_t channel = 0);

  /**
   * @brief Build the ASM file/timestamp info list from op_locations
   * @param opLocations Vector of op_loc from aiebu_assembler::get_op_locations
   * @return Vector of ASMFileInfo (UC spans applied); empty if none found
   */
  std::vector<ASMFileInfo> buildAsmFileInfoList(
      const std::vector<aiebu::aiebu_assembler::op_loc>& opLocations);

  /**
   * @brief Append interface-tile bandwidth counters and begin-block writes
   * @param hwctx Hardware context handle for shim column discovery
   * @param metricSet Bandwidth metric set
   * @param channel DMA channel for detailed_ddr_*_bandwidth sets
   * @param counters [in,out] Accumulated counter list
   * @param beginWrites [in,out] Accumulated begin-block register writes
   * @return true if bandwidth config was appended
   */
  bool appendBandwidthConfig(void* hwctx, const std::string& metricSet, uint8_t channel,
      std::vector<CTCounterInfo>& counters, std::vector<CTRegisterWrite>& beginWrites);

  /**
   * @brief Append the single core-tile compute_io_bound counters and begin-block writes
   * @param cfg Resolved compute_io_bound core configuration
   * @param counters [in,out] Accumulated counter list
   * @param beginWrites [in,out] Accumulated begin-block register writes
   */
  void appendComputeIoBoundConfig(const ComputeIoCoreConfig& cfg,
      std::vector<CTCounterInfo>& counters, std::vector<CTRegisterWrite>& beginWrites);

  /**
   * @brief Generate PC-event + performance counter config for a single core tile
   *
   * Programs PC_Event0-3 (with the Valid bit), resets performance counters 2/3,
   * and configures Performance_Ctrl1 so counter 2 measures kernelWrapper (Compute)
   * and counter 3 measures total (PC_Range_2-3 over [0, PROG_MEM_END]).
   * - Reloadable (cfg.useStartStop=false): counter 2 = PC_Range_0-1 over [startPc, end].
   * - Static-inline (cfg.useStartStop=true): counter 2 Start=PC_0@startPc, Stop=PC_1@stopPc.
   *
   * @param column Partition-relative core tile column
   * @param cfg Resolved compute_io_bound core configuration (provides absRow, mode, PCs)
   * @return Vector of register writes for the begin block
   */
  std::vector<CTRegisterWrite> generatePcRangeCoreConfig(uint8_t column, const ComputeIoCoreConfig& cfg);

  /**
   * @brief Write a self-contained counter CT file with begin-block register writes
   * @param asmFileInfoList Vector of ASMFileInfo with timestamps
   * @param allCounters Vector of all CTCounterInfo for metadata
   * @param beginBlockWrites Vector of register writes for begin block
   * @param outputPath Full path for the output CT file
   * @return true if file was written successfully
   */
  bool writeCounterCTFile(const std::vector<ASMFileInfo>& asmFileInfoList,
                          const std::vector<CTCounterInfo>& allCounters,
                          const std::vector<CTRegisterWrite>& beginBlockWrites,
                          const std::string& outputPath);

private:
  VPDatabase* db;
  std::shared_ptr<AieDtraceMetadata> metadata;
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

  // Stream switch and performance counter configuration offsets
  static constexpr uint64_t STREAM_SWITCH_EVENT_PORT_SEL_OFFSET = 0x0003FF00;
  static constexpr uint64_t PERF_CTRL_OFFSET = 0x00031000;

  // Core (aie) module offsets for the compute_io_bound metric (aie2ps)
  static constexpr uint64_t CM_PERF_CTRL1     = 0x00037504;  // Counters 2,3 start/stop events
  static constexpr uint64_t CM_PERF_COUNTER0  = 0x00037520;  // Counter 0 (Counter N at +4*N)
  static constexpr uint64_t CM_PC_EVENT0      = 0x00038020;  // PC_Event0 (1..3 at +4 each)
  static constexpr uint32_t PC_EVENT_VALID    = 0x80000000;  // PC_Event Valid bit (bit 31)
  static constexpr uint32_t PC_ADDRESS_MASK   = 0x00003FFF;  // PC_Address field (bits 13:0)
  static constexpr uint32_t PROG_MEM_END      = 0x00003FFF;  // End of 16KB program memory
  static constexpr uint8_t  PC_0_EVENT         = 16;         // XAIE2PS_EVENTS_CORE_PC_0 (breakpoint @ PC_Event0)
  static constexpr uint8_t  PC_1_EVENT         = 17;         // XAIE2PS_EVENTS_CORE_PC_1 (breakpoint @ PC_Event1)
  static constexpr uint8_t  PC_RANGE_0_1_EVENT = 20;         // XAIE2PS_EVENTS_CORE_PC_RANGE_0_1
  static constexpr uint8_t  PC_RANGE_2_3_EVENT = 21;         // XAIE2PS_EVENTS_CORE_PC_RANGE_2_3

  // compute_io_bound configures a single core tile at the first column / first
  // core row (partition-relative col 0, row 3).
  static constexpr uint8_t COMPUTE_IO_CORE_COL = 0;
  static constexpr uint8_t COMPUTE_IO_CORE_ROW = 3;

  // Bandwidth monitoring constants
  static constexpr uint8_t NUM_BANDWIDTH_COUNTERS = 4;
  static constexpr uint8_t SHIM_ROW = 0;
  static constexpr uint8_t PORTS_PER_REGISTER = 4;

  // Output filename
  static constexpr const char* CT_OUTPUT_FILENAME = "aie_profile.ct";
};

} // namespace xdp

#endif // AIE_DTRACE_CT_WRITER_H

