// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2022-2025 Advanced Micro Devices, Inc. All rights reserved

#ifndef AIE_DTRACE_VE2_H
#define AIE_DTRACE_VE2_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "aiebu/aiebu_assembler.h"
#include "core/edge/common/aie_parser.h"
#include "xdp/profile/plugin/aie_profile/aie_profile_impl.h"
#include "xdp/profile/plugin/aie_profile/util/aie_profile_util.h"
#include "xdp/profile/plugin/aie_dtrace/util/aie_dtrace_util.h"
#include "xaiefal/xaiefal.hpp"

extern "C" {
#include <xaiengine.h>
#include <xaiengine/xaiegbl_params.h>
}

namespace xdp {
  using tile_type = xdp::tile_type;
  
  class AieDtrace_VE2Impl : public AieProfileImpl{
    public:
      // AieDtrace_VE2Impl(VPDatabase* database, std::shared_ptr<AieProfileMetadata> metadata)
      //   : AieProfileImpl(database, metadata){}
      AieDtrace_VE2Impl(VPDatabase* database, std::shared_ptr<AieProfileMetadata> metadata, uint64_t deviceID);

      ~AieDtrace_VE2Impl() override;

      void updateDevice();

      void startPoll(const uint64_t id) override;
      void continuePoll(const uint64_t id) override;
      void poll(const uint64_t id) override;
      void endPoll() override;

      void freeResources();
      void generateCTForRun(void* run, void* hwctx, uint32_t run_uid,
                           const std::string& kernel_name,
                           void* elf_handle) override;
      bool checkAieDevice(const uint64_t deviceId, void* handle);

      bool setMetricsSettings(const uint64_t deviceId, void* handle);
      void printTileModStats(xaiefal::XAieDev* aieDevice, 
                             const tile_type& tile, 
                             const XAie_ModuleType mod);

      uint64_t getCounterPayload(XAie_DevInst* aieDevInst,
                                 const tile_type& tile,
                                 const module_type type,
                                 uint8_t column, 
                                 uint8_t row, 
                                 uint16_t startEvent,
                                 const std::string metricSet,
                                 const uint8_t channel,
                                 uint8_t logicalPortIndex = 0);
      uint64_t getAdfProfileAPIPayload(const tile_type& tile, const std::string metricSet);

    private:
      std::pair<uint16_t, uint16_t>
      getEventPhysicalId(XAie_LocType& tileLoc,
                         XAie_ModuleType& xaieModType, module_type xdpModType, 
                         const std::string& metricSet,
                         XAie_Events startEvent, XAie_Events endEvent);
  
      std::pair<int, XAie_Events>
      setupBroadcastChannel(const tile_type& currTileLoc);

      inline std::shared_ptr<xaiefal::XAiePerfCounter>
      startCounter(std::shared_ptr<xaiefal::XAiePerfCounter>& pc,
                   XAie_Events counterEvent, XAie_Events& retCounterEvent)
      {
        if (!pc)
          return nullptr;
        
        auto ret = pc->start();
        if (ret != XAIE_OK)
          return nullptr;
        
        // Return the known counter event
        retCounterEvent = counterEvent;
        return pc;
      }

      std::pair<int, XAie_Events>
      getShimBroadcastChannel(const tile_type& srcTile);

      // Stop/release configured FAL objects only (no counter read / sample offload).
      void releaseConfiguredHwResourcesNoRead();

      XAie_DevInst*     aieDevInst = nullptr;
      xaiefal::XAieDev* aieDevice  = nullptr;    

      std::map<std::string, std::vector<XAie_Events>> coreStartEvents;
      std::map<std::string, std::vector<XAie_Events>> coreEndEvents;
      std::map<std::string, std::vector<XAie_Events>> memoryStartEvents;
      std::map<std::string, std::vector<XAie_Events>> memoryEndEvents;
      std::map<std::string, std::vector<XAie_Events>> shimStartEvents;
      std::map<std::string, std::vector<XAie_Events>> shimEndEvents;
      std::map<std::string, std::vector<XAie_Events>> memTileStartEvents;
      std::map<std::string, std::vector<XAie_Events>> memTileEndEvents; 
      std::map<std::string, std::vector<uint32_t>> microcontrollerEvents;
      std::map<tile_type, std::vector<uint32_t>> microcontrollerTileEvents;
      std::vector<std::shared_ptr<xaiefal::XAiePerfCounter>> perfCounters;
      std::vector<std::shared_ptr<xaiefal::XAieStreamPortSelect>> streamPorts;

      bool graphItrBroadcastConfigDone = false;
      // Graph Iterator broadcast channel event
      // This event is used to reset/configure the counters in interface tiles
      XAie_Events graphIteratorBrodcastChannelEvent = XAIE_EVENT_NONE_CORE;

      // This event is asserted in another interface tile
      XAie_Events latencyUserBrodcastChannelEvent = XAIE_EVENT_NONE_CORE;

      std::map<aie::profile::adfAPI, std::map<std::string, aie::profile::adfAPIResourceInfo>> adfAPIResourceInfoMap;
      
      // This stores the map of location of tile and configured broadcast channel event
      std::map<std::string, std::pair<int, XAie_Events>> adfAPIBroadcastEventsMap;

      std::vector<std::shared_ptr<xaiefal::XAieBroadcast>> bcResourcesBytesTx;
      std::vector<std::shared_ptr<xaiefal::XAieBroadcast>> bcResourcesLatency;

      std::map<std::string, std::vector<aiebu::aiebu_assembler::op_loc>> m_op_locations_cache;

      void computeOpLocations(void* elf_handle, const std::string& kernel_name);
  };
}   

#endif
