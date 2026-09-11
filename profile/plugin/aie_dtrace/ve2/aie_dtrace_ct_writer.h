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
#include "xdp/profile/plugin/aie_dtrace/util/aie_dtrace_util.h"

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
 *
 * A mask other than 0xFFFFFFFF emits mask_write_reg instead of write_reg, which
 * is required for registers whose remaining fields belong to someone else.
 */
struct CTRegisterWrite {
  uint64_t address;
  uint32_t value;
  std::string comment;
  uint32_t mask = 0xFFFFFFFF;
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
 * @brief A set of broadcast channels and the one direction they may leave by
 *
 * A module can carry several such groups at once, each headed a different way. They are
 * programmed together so that every direction is written once, with the union of the
 * groups it blocks, rather than once per group.
 */
struct BroadcastChannelGroup {
  uint32_t channels;  // Mask of the broadcast channels in this group
  int openDir;        // Direction to leave unblocked, or -1 to block all four
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
   * @brief Generate a self-contained CT file combining bandwidth and/or a
   *        core-tile metric set into a single begin block + counter reads.
   *
   * Either family can be enabled independently; when both are enabled the shim
   * bandwidth counters and the core-tile counters are emitted into the same CT
   * file.
   *
   * @param outputPath Full path for the generated CT file
   * @param hwctx Hardware context handle for partition info access
   * @param opLocations Vector of op_loc from aiebu_assembler::get_op_locations
   * @param includeBandwidth Emit interface-tile bandwidth counters
   * @param bandwidthMetricSet Bandwidth metric set (used when includeBandwidth)
   * @param bandwidthChannel DMA channel for detailed_ddr_*_bandwidth sets
   * @param coreMetricSet Core (aie) tile metric set to emit, or empty for none.
   *                      Supported: compute_io_bound
   * @return true if CT file was generated successfully, false otherwise
   */
  bool generateCT(const std::string& outputPath,
                  void* hwctx,
                  const std::vector<aiebu::aiebu_assembler::op_loc>& opLocations,
                  bool includeBandwidth,
                  const std::string& bandwidthMetricSet,
                  uint8_t bandwidthChannel,
                  const std::string& coreMetricSet);

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
   * @brief Append the compute_io_bound counters and begin-block writes
   * @param counters [in,out] Accumulated counter list
   * @param beginWrites [in,out] Accumulated begin-block register writes
   */
  void appendComputeIoBoundConfig(std::vector<CTCounterInfo>& counters,
      std::vector<CTRegisterWrite>& beginWrites);

  /**
   * @brief Append memtile L2-L2 counters and begin-block writes from design points
   * @param hwctx Hardware context handle for partition column bounds
   * @param counters [in,out] Accumulated counter list
   * @param beginWrites [in,out] Accumulated begin-block register writes
   */
  void appendL2L2Config(void* hwctx,
      std::vector<CTCounterInfo>& counters,
      std::vector<CTRegisterWrite>& beginWrites);

  /**
   * @brief Generate the core module config for the compute_io_bound lock correlation tile
   *
   * Drives the core's lock stall onto a broadcast channel and confines it to the tile, so
   * the memory module can combine it with its own DMA starvation events. Counters 2 and 3
   * then count the other half of the correlation locally: the lock stall ANDed with each
   * S2MM channel's memory backpressure, arriving from the memory module as broadcasts.
   * Both use Start == Stop so each accumulates the cycles its combo is asserted. This
   * module hosts the backpressure pair because the memory module's two combos are both
   * spent on the starvation pair.
   *
   * @param column Partition-relative core tile column
   * @param row Absolute core tile row
   * @return Vector of register writes for the begin block
   */
  std::vector<CTRegisterWrite> generateLockStarvationCoreConfig(uint8_t column, uint8_t row);

  /**
   * @brief Generate the memory module config for the compute_io_bound lock correlation tile
   *
   * Counters 0 and 1 count two combo events: the broadcast lock stall ANDed with S2MM
   * channel 0 starvation, and the same ANDed with S2MM channel 1 starvation. Each counter
   * uses Start == Stop so it accumulates the cycles its combo is asserted. Counters 2 and
   * 3 are left unprogrammed. This module also drives each S2MM channel's memory
   * backpressure onto a broadcast channel for the core module to pair with the same lock
   * stall.
   *
   * @param column Partition-relative core tile column
   * @param row Absolute core tile row
   * @return Vector of register writes for the begin block
   */
  std::vector<CTRegisterWrite> generateLockStarvationMemoryConfig(uint8_t column, uint8_t row);

  /**
   * @brief Generate the core module config for the compute_io_bound tile
   *
   * Counters 2 and 3 count total execution cycles (PC_Range_2-3 over [0, PROG_MEM_END])
   * and group stall, each with Start == Stop so it accumulates the cycles its event is
   * asserted. Only Performance_Control1 is written, so counters 0 and 1 and their shared
   * Performance_Control0 are left untouched. Also drives the four individual stall events
   * onto broadcast channels and blocks them everywhere but east, the internal link to the
   * memory module.
   *
   * @param column Partition-relative core tile column
   * @param row Absolute core tile row
   * @return Vector of register writes for the begin block
   */
  std::vector<CTRegisterWrite> generateComputeCoreConfig(uint8_t column, uint8_t row);

  /**
   * @brief Generate the memory module config for the compute_io_bound tile
   *
   * Counters 0-3 count the memory, stream, cascade and lock stall events arriving from
   * the core module as broadcasts. The broadcast is blocked in all four directions:
   * blocking west does not stop the module observing the event locally, it only stops it
   * re-driving the signal back into the core module.
   *
   * @param column Partition-relative core tile column
   * @param row Absolute core tile row
   * @return Vector of register writes for the begin block
   */
  std::vector<CTRegisterWrite> generateComputeMemoryConfig(uint8_t column, uint8_t row);

  /**
   * @brief Append the broadcast block writes for a module's compute_io_bound channels
   *
   * Each direction is written at most once: its Set register takes the union of every
   * group that must not escape that way, and its Clr register the union of the groups
   * headed through it. Taking all the groups at once is what keeps a direction from being
   * written twice, which would otherwise leave the file relying on the Set/Clr registers
   * being write-1-to-set and write-1-to-clear to accumulate rather than replace.
   *
   * @param blockBase Module's Event_Broadcast_Block_South_Set offset
   * @param groups The channel groups this module carries
   * @param loc Human-readable location for the generated comments
   * @param writes [in,out] Accumulated begin-block register writes
   */
  void appendBroadcastBlockConfig(uint64_t blockBase,
      const std::vector<BroadcastChannelGroup>& groups, const std::string& loc,
      uint64_t tileAddress, std::vector<CTRegisterWrite>& writes);

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

  std::vector<CTRegisterWrite> generateMemtilePerfCounterConfig(
      uint8_t column,
      const std::vector<aie::dtrace::L2L2CounterPoint>& counterPoints);

private:
  VPDatabase* db;
  std::shared_ptr<AieDtraceMetadata> metadata;
  uint64_t deviceId;

  // AIE configuration values
  uint8_t columnShift;
  uint8_t rowShift;
  uint8_t coreRowStart;       // Absolute row of the first AIE core row (aie_tile_row_start)
  uint8_t partitionStartCol;  // Absolute start column of the hw_context partition

  // Base offsets by module type
  static constexpr uint64_t CORE_MODULE_BASE_OFFSET   = 0x00037520;
  static constexpr uint64_t MEMORY_MODULE_BASE_OFFSET = 0x00011020;
  static constexpr uint64_t MEM_TILE_BASE_OFFSET      = 0x00091020;
  static constexpr uint64_t SHIM_TILE_BASE_OFFSET     = 0x00031020;

  // Stream switch and performance counter configuration offsets
  static constexpr uint64_t STREAM_SWITCH_EVENT_PORT_SEL_OFFSET = 0x0003FF00;
  static constexpr uint64_t MEM_TILE_PERF_CTRL0_OFFSET          = 0x00091000;
  static constexpr uint64_t MEM_TILE_PERF_CTRL1_OFFSET          = 0x00091004;
  static constexpr uint64_t PERF_CTRL_OFFSET = 0x00031000;

  static constexpr uint8_t PORT_RUNNING_0_MEM_TILE_EVENT = 80;  // PORT_RUNNING_N = 80 + 4*N
  static constexpr uint8_t PORT_STALLED_0_MEM_TILE_EVENT = 81;  // PORT_STALLED_N = 81 + 4*N

  // Core (aie) module offsets for the compute_io_bound metric (aie2ps).
  // Performance_Control0 (0x00037500) is deliberately never written: it holds counter
  // 0's start/stop events, and the driver's ECC scrubbing owns core counter 0
  // (XAIE_ECC_PERFCOUNTER_ID in xaie_ecc.c). Counters programmed there did not stick.
  static constexpr uint64_t CM_PERF_CTRL1     = 0x00037504;  // Counters 2,3 start/stop events
  static constexpr uint64_t CM_PERF_COUNTER0  = 0x00037520;  // Counter 0 (Counter N at +4*N)
  static constexpr uint64_t CM_PC_EVENT0      = 0x00038020;  // PC_Event0 (1..3 at +4 each)
  static constexpr uint64_t CM_EVENT_GROUP_CORE_STALL_ENABLE = 0x00034508;
  static constexpr uint32_t PC_EVENT_VALID    = 0x80000000;  // PC_Event Valid bit (bit 31)
  static constexpr uint32_t PC_ADDRESS_MASK   = 0x00003FFF;  // PC_Address field (bits 13:0)
  static constexpr uint32_t PROG_MEM_END      = 0x00003FFF;  // End of 16KB program memory

  // Memory module offsets (aie2ps). The bundled aie-codegen register database only
  // describes two memory module counters, so these come from the aie2ps spec. All four
  // memory module counters are free on AIE tiles: ECC only claims the memory module
  // counter 0 of MEM tiles (_XAie_EccPerfCntConfigMemTile), not of AIE tiles.
  static constexpr uint64_t MM_PERF_CTRL0     = 0x00011000;  // Counters 0,1 start/stop events
  static constexpr uint64_t MM_PERF_CTRL2     = 0x0001100C;  // Counters 2,3 start/stop events
  static constexpr uint64_t MM_PERF_COUNTER0  = 0x00011020;  // Counter 0 (Counter N at +4*N)

  // Restrict Group_Core_Stall to the four stalls that are also counted individually,
  // dropping the reset value's debug/active/disable/ECC contributors.
  static constexpr uint32_t GROUP_CORE_STALL_MASK = 0x0000000F;

  // aie2ps core module events (xaie_events_aie2ps.h)
  static constexpr uint8_t  PC_RANGE_2_3_EVENT = 21;
  static constexpr uint8_t  GROUP_STALL_EVENT  = 22;
  static constexpr uint8_t  MEMORY_STALL_EVENT = 23;
  static constexpr uint8_t  STREAM_STALL_EVENT = 24;
  static constexpr uint8_t  CASCADE_STALL_EVENT = 25;
  static constexpr uint8_t  LOCK_STALL_EVENT   = 26;

  // Memory module event Broadcast_N is MEM_BROADCAST_0_EVENT + N, and likewise
  // CORE_BROADCAST_0_EVENT + N in the core module.
  static constexpr uint8_t  MEM_BROADCAST_0_EVENT  = 107;
  static constexpr uint8_t  CORE_BROADCAST_0_EVENT = 107;

  // Event_Broadcast0 registers (channel N at +4*N) and the broadcast block registers,
  // laid out as base + direction * BCAST_BLOCK_DIR_STRIDE with Set at +0 and Clr at +4.
  static constexpr uint64_t CM_EVENT_BROADCAST0    = 0x00034010;
  static constexpr uint64_t MM_EVENT_BROADCAST0    = 0x00014010;
  static constexpr uint64_t CM_BCAST_BLOCK_BASE    = 0x00034050;
  static constexpr uint64_t MM_BCAST_BLOCK_BASE    = 0x00014050;
  static constexpr uint64_t BCAST_BLOCK_DIR_STRIDE = 0x10;
  static constexpr uint64_t BCAST_BLOCK_CLR_OFFSET = 4;
  static constexpr int      BCAST_DIR_SOUTH = 0;
  static constexpr int      BCAST_DIR_WEST  = 1;
  static constexpr int      BCAST_DIR_NORTH = 2;
  static constexpr int      BCAST_DIR_EAST  = 3;
  static constexpr int      BCAST_NUM_DIRS  = 4;

  // Broadcast channels carrying the four individual stall events to the memory module.
  // Channels 0-2 are reserved by the driver for error/UC/user events, and channel 6 by
  // ECC; the FAL resource manager hands out low ids, so these count down from the top.
  static constexpr uint8_t  STREAM_STALL_BCAST_CHANNEL  = 15;
  static constexpr uint8_t  CASCADE_STALL_BCAST_CHANNEL = 14;
  static constexpr uint8_t  LOCK_STALL_BCAST_CHANNEL    = 13;
  static constexpr uint8_t  MEMORY_STALL_BCAST_CHANNEL  = 12;

  // Broadcast channels carrying the two S2MM memory backpressure events the other way,
  // from the memory module to the core module. They continue the block above so this
  // metric owns a contiguous 10-15, clear of the driver's channels and of the fixed 6-9
  // the aie_trace windowed-trace and core-to-memory networks use.
  static constexpr uint8_t  S2MM_BP_CH0_BCAST_CHANNEL = 11;
  static constexpr uint8_t  S2MM_BP_CH1_BCAST_CHANNEL = 10;

  // The two directions are blocked independently, so each carries its own channel mask.
  static constexpr uint32_t STALL_BCAST_CHANNELS =
      (1u << STREAM_STALL_BCAST_CHANNEL) | (1u << CASCADE_STALL_BCAST_CHANNEL)
    | (1u << LOCK_STALL_BCAST_CHANNEL)   | (1u << MEMORY_STALL_BCAST_CHANNEL);
  static constexpr uint32_t S2MM_BP_BCAST_CHANNELS =
      (1u << S2MM_BP_CH0_BCAST_CHANNEL) | (1u << S2MM_BP_CH1_BCAST_CHANNEL);

  // compute_io_bound uses a single tile: core module counters 2 and 3 for total
  // execution cycles and group stall, memory module counters 0-3 for the four
  // individual stalls arriving as broadcasts. Only registers this metric fully owns are
  // written, so no counter shares a control register with another owner.
  static constexpr uint8_t COMPUTE_IO_CORE_COL = 0;

  // compute_io_bound also programs a second tile, one core row above the first, which
  // pairs the core's lock stall with each S2MM channel's starvation on its memory module
  // counters, and with each channel's memory backpressure on its core module counters.
  // Channel 0 carries the IFM and channel 1 the weights, so the counters say which input
  // the core was waiting on, and whether that input was late from the stream (starvation)
  // or arrived but could not be written into local memory (backpressure).
  static constexpr uint8_t LOCK_STARVATION_ROW_OFFSET = 1;

  // aie2ps memory module DMA events (xaie_events_aie2ps.h), channel N at base + N.
  static constexpr uint8_t MEM_DMA_S2MM_0_STREAM_STARVATION_EVENT   = 35;
  static constexpr uint8_t MEM_DMA_S2MM_0_MEMORY_BACKPRESSURE_EVENT = 39;

  // Combo_Event_Inputs packs four 7-bit event ids: A at 0, B at 8, C at 16, D at 24.
  // Combo_Event_Control holds one 2-bit op per combo, 8 bits apart, and 0 selects AND
  // (XAIE_EVENT_COMBO_E1_AND_E2). Combo 0 is A op B and combo 1 is C op D, so a module
  // has only two independent pairs: combo 2 just recombines those two results.
  static constexpr uint64_t MM_COMBO_EVENT_INPUTS  = 0x00014400;
  static constexpr uint64_t MM_COMBO_EVENT_CONTROL = 0x00014404;
  static constexpr uint64_t CM_COMBO_EVENT_INPUTS  = 0x00034400;
  static constexpr uint64_t CM_COMBO_EVENT_CONTROL = 0x00034404;
  static constexpr uint32_t COMBO_AND = 0;
  static constexpr uint8_t  MEM_COMBO_EVENT_0_EVENT  = 7;
  static constexpr uint8_t  MEM_COMBO_EVENT_1_EVENT  = 8;
  static constexpr uint8_t  CORE_COMBO_EVENT_0_EVENT = 9;
  static constexpr uint8_t  CORE_COMBO_EVENT_1_EVENT = 10;

  // Bandwidth monitoring constants
  static constexpr uint8_t NUM_BANDWIDTH_COUNTERS = 4;
  static constexpr uint8_t SHIM_ROW = 0;
  static constexpr uint8_t PORTS_PER_REGISTER = 4;

  // Output filename
  static constexpr const char* CT_OUTPUT_FILENAME = "aie_profile.ct";
};

} // namespace xdp

#endif // AIE_DTRACE_CT_WRITER_H

