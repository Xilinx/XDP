// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#ifndef AIE_DTRACE_METADATA_H
#define AIE_DTRACE_METADATA_H

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/database/static_info/filetypes/base_filetype_impl.h"

namespace xdp {

class AieDtraceMetadata {
  private:
    static constexpr int SHIM_MODULE_IDX = static_cast<int>(module_type::shim);
    static constexpr int CORE_MODULE_IDX = static_cast<int>(module_type::core);
    static constexpr int NUM_MODULES = static_cast<int>(module_type::num_types);

    // The compute_io_bound metric configures a single fixed core tile at the
    // first column / first core row (absolute location col 0, row 3).
    static constexpr uint8_t COMPUTE_IO_CORE_COL = 0;
    static constexpr uint8_t COMPUTE_IO_CORE_ROW = 3;

    uint64_t deviceID = 0;
    double clockFreqMhz = 0.0;
    void* handle = nullptr;
    bool configOnePartition = false;

    std::vector<std::map<tile_type, std::string>> configMetrics;
    std::map<tile_type, uint8_t> configChannel0;
    std::map<tile_type, uint8_t> configChannel1;

    const aie::BaseFiletypeImpl* metadataReader = nullptr;

    void checkDtraceSettings();
    void getConfigMetricsForInterfaceTiles(int moduleIdx,
                                           const std::vector<std::string>& metricsSettings);
    void getConfigMetricsForAIETiles(int moduleIdx,
                                      const std::vector<std::string>& metricsSettings);
    bool isBandwidthMetricSet(const std::string& metricSet) const;
    bool isCoreMetricSet(const std::string& metricSet) const;

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
      return shimConfigured || coreConfigured;
    }

    bool isConfigOnePartition() const { return configOnePartition; }

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

    // Wrapper PC (reloadable ELF entry PC) for the compute_io_bound core tile.
    // Returns std::nullopt when the col0/row0 elfs_metadata entry is missing or
    // its reloadable_elfs values are not all identical.
    std::optional<uint32_t> getWrapperPC() const {
      return metadataReader == nullptr ? std::nullopt
                                       : metadataReader->getReloadableElfEntryPC();
    }

    std::unique_ptr<const AIEProfileFinalConfig> createAIEProfileConfig();
};

} // namespace xdp

#endif
