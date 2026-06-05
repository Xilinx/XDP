// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2022-2026 Advanced Micro Devices, Inc. All rights reserved

#ifndef AIE_DTRACE_METADATA_H
#define AIE_DTRACE_METADATA_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/database/static_info/filetypes/base_filetype_impl.h"

namespace xdp {

class AieDtraceMetadata {
  private:
    static constexpr int SHIM_MODULE_IDX = static_cast<int>(module_type::shim);
    static constexpr int NUM_MODULES = static_cast<int>(module_type::num_types);

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
    bool isBandwidthMetricSet(const std::string& metricSet) const;

  public:
    AieDtraceMetadata(uint64_t deviceID, void* handle);

    uint64_t getDeviceID() { return deviceID; }
    void* getHandle() { return handle; }

    bool isConfigured() const {
      return SHIM_MODULE_IDX < static_cast<int>(configMetrics.size())
          && !configMetrics[SHIM_MODULE_IDX].empty();
    }

    bool isConfigOnePartition() const { return configOnePartition; }

    bool aieMetadataEmpty() { return metadataReader == nullptr; }

    std::vector<std::string> getSettingsVector(std::string settingsString);

    std::vector<std::pair<tile_type, std::string>> getConfigMetricsVec(int module);

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
