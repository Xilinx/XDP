// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#ifndef AIE_DTRACE_METADATA_H
#define AIE_DTRACE_METADATA_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/database/static_info/filetypes/base_filetype_impl.h"
#include "xdp/profile/plugin/vp_base/profiling_runtime_config.h"

namespace xdp {

// Everything that distinguishes one inference's CT file from another's. The
// Nth profiled inference of a kernel is programmed with metricSelections[N],
// so this has to be self-contained rather than something the CT writer reads
// back off the shared, whole-context config maps.
struct MetricSelection {
  bool includeBandwidth = false;
  std::string bandwidthMetricSet = "peak_read_bandwidth";
  uint8_t bandwidthChannel = 0;
  std::string coreMetricSet;     // empty means no core (aie) tile metrics
  bool includeL2L2 = false;
  std::string memTileMetricSet;  // empty means no per-tile mem tile counters
  uint8_t memTileChannel = 0;    // MM2S channel for output/mm2s_channels_details
  // Empty means every mem tile column in the partition. A "<column>:<metric>"
  // prefix lists those columns here.
  std::vector<uint8_t> memTileColumns;

  bool empty() const {
    return !includeBandwidth && coreMetricSet.empty() && !includeL2L2
        && memTileMetricSet.empty();
  }

  // Human-readable "interface_tile=..., aie_tile=..." form used in log
  // messages and to distinguish CT files in diagnostics.
  std::string describe() const;
};

class AieDtraceMetadata {
  private:
    static constexpr int SHIM_MODULE_IDX = static_cast<int>(module_type::shim);
    static constexpr int CORE_MODULE_IDX = static_cast<int>(module_type::core);
    static constexpr int MEM_TILE_MODULE_IDX = static_cast<int>(module_type::mem_tile);
    static constexpr int NUM_MODULES = static_cast<int>(module_type::num_types);

    // Placeholder tile used only as the config-map key that enables the metric.
    // The CT writer picks the core tiles the chosen metric set actually programs
    // and derives their absolute rows from driver_config.aie_tile_row_start.
    static constexpr uint8_t CORE_METRIC_COL = 0;
    static constexpr uint8_t CORE_METRIC_ROW = 3;

    // Mem tile placeholder column, used when the setting asks for every column.
    // The CT writer expands it against the partition and derives the rows from
    // driver_config.mem_row_start / mem_num_rows.
    static constexpr uint8_t MEM_TILE_METRIC_COL = 0;

    // A mem tile DMA has six MM2S channels, and the selection register field is
    // three bits wide.
    static constexpr uint8_t NUM_MEM_TILE_DMA_CHANNELS = 6;

    uint64_t deviceID = 0;
    double clockFreqMhz = 0.0;
    void* handle = nullptr;
    bool configOnePartition = false;
    bool l2L2TransferEnabled = false;

    // True when the mem tile setting asked for every column rather than naming
    // one, in which case configMetrics holds a single placeholder entry.
    bool memTileAllColumns = false;

    std::vector<std::map<tile_type, std::string>> configMetrics;
    std::map<tile_type, uint8_t> configChannel0;
    std::map<tile_type, uint8_t> configChannel1;

    // One entry per inference to profile, in execution order. Always holds at
    // least one entry once the metadata is configured.
    std::vector<MetricSelection> metricSelections;
    unsigned int startInference = 1;
    bool multiInference = false;

    const aie::BaseFiletypeImpl* metadataReader = nullptr;

    void checkDtraceSettings();
    MetricSelection buildSelectionFromProfileRun(
        const profiling_runtime_config::profile_run_t& run, size_t index) const;
    MetricSelection buildSelectionFromConfigMetrics();
    void getConfigMetricsForInterfaceTiles(int moduleIdx,
                                           const std::vector<std::string>& metricsSettings);
    void getConfigMetricsForAIETiles(int moduleIdx,
                                      const std::vector<std::string>& metricsSettings);
    void getConfigMetricsForMemTiles(int moduleIdx,
                                      const std::vector<std::string>& metricsSettings);
    bool isBandwidthMetricSet(const std::string& metricSet) const;
    bool isCoreMetricSet(const std::string& metricSet) const;
    bool isMemTileMetricSet(const std::string& metricSet) const;

  public:
    AieDtraceMetadata(uint64_t deviceID, void* handle);

    uint64_t getDeviceID() { return deviceID; }
    void* getHandle() { return handle; }

    bool isConfigured() const {
      const int numModules = static_cast<int>(configMetrics.size());
      const bool shimConfigured = SHIM_MODULE_IDX < numModules
          && !configMetrics[SHIM_MODULE_IDX].empty();
      const bool coreConfigured = CORE_MODULE_IDX < numModules
          && !configMetrics[CORE_MODULE_IDX].empty();
      const bool memTileConfigured = MEM_TILE_MODULE_IDX < numModules
          && !configMetrics[MEM_TILE_MODULE_IDX].empty();
      return shimConfigured || coreConfigured || memTileConfigured;
    }

    bool isConfigOnePartition() const { return configOnePartition; }

    // Per-inference metric selections, in execution order.
    const std::vector<MetricSelection>& getMetricSelections() const { return metricSelections; }

    // 1-based index of the first inference of a kernel that gets profiled.
    unsigned int getStartInference() const { return startInference; }

    // True when the user asked for a "profile_runs" sequence rather than a
    // single configuration applied to every inference.
    bool isMultiInference() const { return multiInference; }

    bool aieMetadataEmpty() { return metadataReader == nullptr; }

    std::vector<std::string> getSettingsVector(std::string settingsString);

    std::vector<std::pair<tile_type, std::string>> getConfigMetricsVec(int module);

    // DMA channel selected by each interface tile metric's ":<channel>" suffix;
    // used by detailed_ddr_*_bandwidth to pick the MM2S/S2MM channel to monitor.
    std::map<tile_type, uint8_t> getConfigChannel0() { return configChannel0; }

    int getHardwareGen() const {
      return metadataReader == nullptr ? 0 : metadataReader->getHardwareGeneration();
    }

    double getClockFreqMhz() { return clockFreqMhz; }

    std::vector<uint8_t> getPartitionOverlayStartCols() const {
      return metadataReader->getPartitionOverlayStartCols();
    }

    aie::driver_config getAIEConfigMetadata();

    std::unique_ptr<const AIEProfileFinalConfig> createAIEProfileConfig();
};

} // namespace xdp

#endif
