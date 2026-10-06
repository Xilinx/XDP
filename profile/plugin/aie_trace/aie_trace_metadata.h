/**
 * Copyright (C) 2022-2026 Advanced Micro Devices, Inc. - All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#ifndef AIE_TRACE_METADATA_H
#define AIE_TRACE_METADATA_H

#include <boost/property_tree/ptree.hpp>
#include <climits>
#include <set>
#include <map>
#include <vector>

#include "xdp/config.h"
#include "xdp/profile/database/static_info/aie_util.h"
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/database/static_info/filetypes/base_filetype_impl.h"

#include "core/common/device.h"
#include "core/common/message.h"
#include "core/common/system.h"
#include "core/include/xrt/xrt_hw_context.h"
#include "xdp/profile/database/parser/metrics.h"
#include "xdp/profile/database/parser/metrics_collection_manager.h"
#include "xdp/profile/database/parser/json_parser.h"
#include "xdp/profile/plugin/vp_base/profiling_runtime_config.h"

namespace xdp {

typedef std::vector<uint32_t>  ValueVector;

class AieTraceMetadata {
  public:
    AieTraceMetadata(uint64_t deviceID, void* handle);

    void checkSettings();
    void setTraceStartControl(bool graphIteratorEvent);
    std::vector<std::string> getSettingsVector(std::string settingsString);
    uint8_t getMetricSetIndex(std::string metricString);
    
    void getConfigMetricsForTiles(std::vector<std::string>& metricsSettings,
                                  std::vector<std::string>& graphMetricsSettings,
                                  module_type type);
    void getConfigMetricsForInterfaceTiles(const std::vector<std::string>& metricsSettings,
                                           const std::vector<std::string> graphMetricsSettings);
    xdp::aie::driver_config getAIEConfigMetadata();

    // JSON-based configuration methods
    void getConfigMetricsUsingJson(const int module, const module_type type,
                                   MetricsCollectionManager& metricsCollectionManager);
    void getConfigMetricsForTilesUsingJson(const int moduleIdx, const module_type mod,
                                           MetricsCollectionManager& metricsCollectionManager);
    void getConfigMetricsForInterfaceTilesUsingJson(const int moduleIdx,
                                                    MetricsCollectionManager& metricsCollectionManager);
    void populateGraphConfigMetricsForTilesUsingJson(const int moduleIdx, const module_type mod,
                                                     MetricsCollectionManager& metricsCollectionManager);
    void populateTilesConfigMetricsForTilesUsingJson(const int moduleIdx, const module_type mod,
                                                     MetricsCollectionManager& metricsCollectionManager);
    void populateGraphConfigMetricsForInterfaceTilesUsingJson(const int moduleIdx, const module_type mod,
                                                              MetricsCollectionManager& metricsCollectionManager);
    void populateTilesConfigMetricsForInterfaceTilesUsingJson(const int moduleIdx, const module_type mod,
                                                             MetricsCollectionManager& metricsCollectionManager);
    
    // Plugin JSON settings processing
    void processPluginJsonSetting(const PluginJsonSetting& config, 
                                  MetricsCollectionManager& manager);

  private:
    void resolveSettings();

   public:
    int getHardwareGen() {
      if (metadataReader)
        return metadataReader->getHardwareGeneration();
      return 0;
    }
    uint8_t getRowOffset() {
      if (metadataReader)
        return metadataReader->getAIETileRowOffset();
      return 0;
    }
    std::unordered_map<std::string, io_config> 
    get_trace_gmios() {
      if (metadataReader)
        return metadataReader->getTraceGMIOs();
      return {};
    }
    std::string getMetricString(uint8_t index) {
      if (index < metricSets[module_type::core].size())
        return metricSets[module_type::core][index];
      else
        return metricSets[module_type::core][0];
    }

    bool getUseDelay(){return useDelay;}
    bool getUseUserControl(){return useUserControl;}
    bool getUseGraphIterator(){return useGraphIterator;}
    bool getUseOneDelayCounter(){return useOneDelayCtr;}
    bool getRuntimeMetrics() {return runtimeMetrics;}
    std::string getCounterScheme(){return counterScheme;}

    // Centralized, precedence-aware accessors for settings that are also
    // read directly from xrt_core::config at various call sites outside
    // this class (ve2/client aie_trace impls, aie_trace_plugin.cpp,
    // aie_trace_offload_ve2.cpp). Resolved once in the constructor so that
    // when Debug.profiling_runtime_config carries an event_trace section,
    // every one of these call sites picks it up instead of re-reading
    // xrt.ini directly.
    std::string getStartTypeSetting() {return startTypeSetting;}
    unsigned int getStartLayer() {return startLayer;}
    bool getTraceStartBroadcast() {return traceStartBroadcast;}
    bool getConfigOnePartitionSetting() {return configOnePartitionSetting;}
    bool getReuseBufferSetting() {return reuseBufferSetting;}
    bool getEnableSystemTimeline() {return enableSystemTimeline;}
    std::string getBufferSizeStr() {return bufferSizeStr;}

    // Static variants of the corresponding getters above, for call sites
    // that don't have (or, per build configuration, can't have) an
    // AieTraceMetadata instance handy: the early multi-partition
    // short-circuit in AieTracePluginUnified::updateAIEDevice runs before
    // metadata exists for the current handle, and the ZOCL-only circular
    // buffer support check in aie_trace_offload_ve2.cpp runs in a build
    // configuration where AIETraceOffload has no metadata member at all
    // (see aie_trace_offload_ve2.h). Both simply delegate to the shared
    // blob-or-ini resolver in profiling_runtime_config so there is exactly
    // one place that decides which source wins for these settings.
    static bool configOnePartitionEnabled()
    {
      return profiling_runtime_config::resolveConfigOnePartition();
    }

    static bool reuseBufferEnabled()
    {
      return profiling_runtime_config::resolveReuseBuffer();
    }

    uint32_t getIterationCount(){return iterationCount;}
    // uint64_t getNumStreams() {return numAIETraceOutput;}
    uint64_t getNumStreamsPLIO() {return numAIETraceOutputPLIO;}
    uint64_t getNumStreamsGMIO() {return numAIETraceOutputGMIO;}
    uint64_t getContinuousTrace() {return continuousTrace;}
    void resetContinuousTrace() {continuousTrace = false;}
    uint64_t getOffloadIntervalUs() {return offloadIntervalUs;}
    uint64_t getDeviceID() {return deviceID;}
    bool getIsValidMetrics() {return isValidMetrics;}

    void* getHandle() {return handle;}
    uint32_t getPollingIntervalVal(){return pollingInterval;}
    uint32_t getMaxTimerSamples(){return maxTimerSamples;}
    unsigned int getFileDumpIntS() {return aie_trace_file_dump_int_s;}
    std::string getMetricStr() {return metricSet;}
    std::map<tile_type, std::string> getConfigMetrics() {return configMetrics;}
    std::map<tile_type, uint8_t> getConfigChannel0() {return configChannel0;}
    std::map<tile_type, uint8_t> getConfigChannel1() {return configChannel1;}

    void setNumStreamsPLIO(uint64_t newNumTraceStreams) {numAIETraceOutputPLIO = newNumTraceStreams;}
    void setNumStreamsGMIO(uint64_t newNumTraceStreams) {numAIETraceOutputGMIO = newNumTraceStreams;}
    void setDelayCycles(uint64_t newDelayCycles) {delayCycles = newDelayCycles;}
    void setRuntimeMetrics(bool metrics) {runtimeMetrics = metrics;}
    uint64_t getDelay() {return ((useDelay) ? delayCycles : 0);}

    xrt::hw_context getHwContext(){return hwContext;}
    void setHwContext(xrt::hw_context c) {
      hwContext = std::move(c);
    }
    inline std::vector<uint8_t> getPartitionOverlayStartCols() const {
      return metadataReader->getPartitionOverlayStartCols();
    }
    bool aieMetadataEmpty() { return metadataReader==nullptr; }

    bool isGMIOMetric(const std::string& metric) const {
      return gmioMetricSets.find(metric) != gmioMetricSets.end();
    }
    bool configMetricsEmpty() const { return configMetrics.empty(); }

  private:
    bool useDelay = false;
    bool useUserControl = false;
    bool useGraphIterator = false;
    bool useOneDelayCtr = true;
    bool isValidMetrics = true;   
    bool runtimeMetrics;
    bool continuousTrace;
    bool invalidXclbinMetadata;

    uint32_t pollingInterval;
    uint32_t maxTimerSamples;
    uint32_t iterationCount = 0;
    uint64_t delayCycles = 0;
    uint64_t deviceID;
    uint64_t numAIETraceOutputPLIO = 0;
    uint64_t numAIETraceOutputGMIO = 0;
    uint64_t offloadIntervalUs = 0;
    unsigned int aie_trace_file_dump_int_s;

    unsigned int startLayer = UINT_MAX;
    bool traceStartBroadcast = true;
    bool configOnePartitionSetting = false;
    bool reuseBufferSetting = false;
    bool enableSystemTimeline = true;
    std::string bufferSizeStr = "8M";

    // Resolved once (from blob or xrt.ini, see resolveStartControlSettings())
    // and consumed by setTraceStartControl() so that method doesn't need to
    // know which source won.
    std::string startTypeSetting = "time";
    std::string startTimeSetting = "0";
    unsigned int startIterationSetting = 1;

    std::string counterScheme;
    std::string metricSet;
    std::map<tile_type, std::string> configMetrics;
    std::map<tile_type, uint8_t> configChannel0;
    std::map<tile_type, uint8_t> configChannel1;
    const aie::BaseFiletypeImpl* metadataReader = nullptr;

    std::map<module_type, std::string> defaultSets {
      { module_type::core,     "functions"},
      { module_type::dma,      "functions"},
      { module_type::mem_tile, "input_channels"},
      { module_type::shim,     "input_ports"}
    };

    std::map <module_type, std::vector<std::string>> metricSets {
      { module_type::core,     {"functions", "partial_stalls",
                                "all_stalls", "all_dma", "all_stalls_dma",
                                "all_stalls_s2mm", "all_stalls_mm2s",
                                "s2mm_channels", "mm2s_channels",
                                "s2mm_channels_stalls", "mm2s_channels_stalls",
                                "execution"} },
      { module_type::mem_tile, {"input_channels", "input_channels_stalls", 
                                "output_channels", "output_channels_stalls",
                                "s2mm_channels", "s2mm_channels_stalls", 
                                "mm2s_channels", "mm2s_channels_stalls",
                                "memory_conflicts1", "memory_conflicts2"} },
      { module_type::shim,     {"input_ports", "output_ports",
                                "input_ports_stalls", "output_ports_stalls", 
                                "input_ports_details", "output_ports_details",
                                "mm2s_ports", "s2mm_ports",
                                "mm2s_ports_stalls", "s2mm_ports_stalls", 
                                "mm2s_ports_details", "s2mm_ports_details",
                                "input_output_ports", "mm2s_s2mm_ports",
                                "input_output_ports_stalls", "mm2s_s2mm_ports_stalls",
                                "uc_dma_dm2mm", "uc_dma_mm2dm", "uc_axis", "uc_dma",
                                "uc_program_flow"} }
    };

    std::set<std::string> gmioMetricSets {
                                "input_ports_details", "output_ports_details",
                                "mm2s_ports_details", "s2mm_ports_details" };

    void* handle;
    xrt::hw_context hwContext;
  };

}

#endif
