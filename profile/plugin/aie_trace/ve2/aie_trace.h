// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2022-2025 Advanced Micro Devices, Inc. All rights reserved

#ifndef AIE_TRACE_DOT_H
#define AIE_TRACE_DOT_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef XDP_USE_AIE_CODEGEN
#include <aie_codegen.h>
#include <aie_codegen_inc/xaiegbl_params.h>
#include "xdp/profile/device/common/ve2/ve2_transaction.h"
#else
#include <xaiengine.h>
#include <xaiengine/xaiegbl_params.h>
#include "xaiefal/xaiefal.hpp"
#endif

#include "xdp/profile/plugin/aie_trace/aie_trace_impl.h"
#include "xdp/profile/plugin/aie_trace/util/aie_trace_config.h"

namespace xdp {

  class AieTrace_VE2Impl : public AieTraceImpl {
  public:
    AieTrace_VE2Impl(VPDatabase* database, std::shared_ptr<AieTraceMetadata> metadata);
    ~AieTrace_VE2Impl() = default;

    // AieTraceImpl overrides (implemented in both flows; behavior differs).
    void updateDevice() override;
    void flushTraceModules() override;
    void pollTimers(uint64_t index, void* handle) override;
    void freeResources() override;
    void* setAieDeviceInst(void* handle, uint64_t deviceID) override;
    uint64_t checkTraceBufSize(uint64_t size) override;

  private:
    // -------------------------------------------------------------------------
    // Shared by ZOCL and XDNA
    // -------------------------------------------------------------------------
    bool setMetricsSettings(uint64_t deviceId, void* handle);

    typedef XAie_Events EventType;
    typedef std::vector<EventType> EventVector;
    typedef std::vector<uint32_t> ValueVector;

    // AIE resources
    std::map<std::string, EventVector> coreEventSets;
    std::map<std::string, EventVector> memoryEventSets;
    std::map<std::string, EventVector> memoryTileEventSets;
    std::map<std::string, EventVector> interfaceTileEventSets;

    // Counter metrics (same for all sets)
    EventType coreTraceStartEvent;
    EventType coreTraceEndEvent;
    EventType memoryTileTraceStartEvent;
    EventType memoryTileTraceEndEvent;
    EventType interfaceTileTraceStartEvent;
    EventType interfaceTileTraceEndEvent;

    EventVector coreCounterStartEvents;
    EventVector coreCounterEndEvents;
    ValueVector coreCounterEventValues;

    EventVector memoryCounterStartEvents;
    EventVector memoryCounterEndEvents;
    ValueVector memoryCounterEventValues;

    EventVector interfaceCounterStartEvents;
    EventVector interfaceCounterEndEvents;
    ValueVector interfaceCounterEventValues;

    // Tile locations to apply trace end and flush
    std::vector<XAie_LocType> traceFlushLocs;
    std::vector<XAie_LocType> memoryTileTraceFlushLocs;
    std::vector<XAie_LocType> interfaceTileTraceFlushLocs;

    // Keep track of number of events reserved per module and/or tile
    int mNumTileTraceEvents[static_cast<int>(module_type::num_types)][NUM_TRACE_EVENTS + 1];

#ifdef XDP_VE2_ZOCL_BUILD
    // -------------------------------------------------------------------------
    // VE2 ZOCL only (FAL + live xaiengine device)
    // -------------------------------------------------------------------------
    bool tileHasFreeRsc(xaiefal::XAieDev* aieDevice, XAie_LocType& loc,
                        const module_type type, const std::string& metricSet);
    bool configureWindowedEventTrace(xaiefal::XAieDev* aieDevice);

    XAie_DevInst* aieDevInst = nullptr;
    xaiefal::XAieDev* aieDevice = nullptr;
    std::vector<std::shared_ptr<xaiefal::XAiePerfCounter>> perfCounters;
    std::vector<std::shared_ptr<xaiefal::XAieStreamPortSelect>> streamPorts;

#else
    // -------------------------------------------------------------------------
    // VE2 XDNA only (control-code / aie_codegen + VE2Transaction)
    // -------------------------------------------------------------------------
    bool configureWindowedEventTrace(void* handle);
    void build2ChannelBroadcastNetwork(void* handle, uint8_t broadcastId1,
                                       uint8_t broadcastId2, XAie_Events event);
    void reset2ChannelBroadcastNetwork(void* handle, uint8_t broadcastId1,
                                       uint8_t broadcastId2);
    uint32_t bcIdToEvent(int bcId);

    void configStreamSwitchPorts(const tile_type& tile, const XAie_LocType loc,
                                 const module_type type, const std::string metricSet,
                                 const uint8_t channel0, const uint8_t channel1,
                                 std::vector<XAie_Events>& events, aie_cfg_base& config);
    std::vector<XAie_Events> configComboEvents(const XAie_LocType loc, const XAie_ModuleType mod,
                                              const module_type type, const std::string metricSet,
                                              aie_cfg_base& config);
    void configGroupEvents(const XAie_LocType loc, const XAie_ModuleType mod,
                          const module_type type, const std::string metricSet);
    void configEventSelections(const tile_type& tile, const XAie_LocType loc,
                               const module_type type, const std::string metricSet,
                               const uint8_t channel0, const uint8_t channel1,
                               aie_cfg_base& config);
    void configEdgeEvents(const tile_type& tile, const module_type type,
                          const std::string metricSet, const XAie_Events event,
                          const uint8_t channel = 0);
    void modifyEvents(module_type type, io_type subtype, const std::string metricSet,
                      uint8_t channel, std::vector<XAie_Events>& events);

    XAie_DevInst aieDevInst = {0};
    std::unique_ptr<aie::VE2Transaction> tranxHandler;
    bool m_trace_start_broadcast = false;
    EventType memoryModTraceStartEvent;
#endif
};

}  // namespace xdp

#endif
