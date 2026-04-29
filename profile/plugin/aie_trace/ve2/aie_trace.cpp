// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2022-2025 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_trace/ve2/aie_trace.h"
#include "xdp/profile/plugin/aie_trace/util/aie_trace_util.h"
#include "xdp/profile/plugin/aie_trace/util/aie_trace_config.h"
#include "xdp/profile/database/static_info/aie_util.h"

#include "xdp/profile/database/database.h"
#include "xdp/profile/database/events/creator/aie_trace_data_logger.h"
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/database/static_info/pl_constructs.h"
#include "xdp/profile/device/pl_device_intf.h"
#include "xdp/profile/device/tracedefs.h"
#include "xdp/profile/plugin/aie_trace/aie_trace_metadata.h"
#include "xdp/profile/plugin/aie_base/aie_base_util.h"
#include "xdp/profile/plugin/vp_base/utility.h"

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <regex>

#include "core/common/message.h"
#include "core/common/time.h"
#include "core/include/xrt/xrt_kernel.h"
#include "core/common/shim/hwctx_handle.h"
#include "core/common/api/hw_context_int.h"
#include "shim_ve2/xdna_hwctx.h"

#ifndef XDP_VE2_ZOCL_BUILD
#include "xdp/profile/device/aie_trace/ve2/aie_trace_offload_ve2.h"
#endif

#ifdef XDP_VE2_ZOCL_BUILD
namespace {
  static void* fetchAieDevInst(void* devHandle)
  {
    xrt::hw_context context = xrt_core::hw_context_int::create_hw_context_from_implementation(devHandle);
    auto hwctx_hdl = static_cast<xrt_core::hwctx_handle*>(context);
    auto hwctx_obj = dynamic_cast<shim_xdna_edge::xdna_hwctx*>(hwctx_hdl);
    auto aieArray = hwctx_obj->get_aie_array();
    return aieArray->get_dev();
  }

  static void* allocateAieDevice(void* devHandle)
  {
    auto aieDevInst = static_cast<XAie_DevInst*>(fetchAieDevInst(devHandle));
    if (!aieDevInst)
      return nullptr;
    return new xaiefal::XAieDev(aieDevInst, false);
  }

  static void deallocateAieDevice(void* aieDevice)
  {
    auto object = static_cast<xaiefal::XAieDev*>(aieDevice);
    if (object != nullptr)
      delete object;
  }
}  // end anonymous namespace

namespace xdp {
  using severity_level = xrt_core::message::severity_level;

  /****************************************************************************
   * Constructor: AIE trace implementation for edge devices
   ***************************************************************************/
  AieTrace_VE2Impl::AieTrace_VE2Impl(VPDatabase* database, std::shared_ptr<AieTraceMetadata> metadata)
      : AieTraceImpl(database, metadata)
  {
    auto hwGen = metadata->getHardwareGen();
    auto counterScheme = metadata->getCounterScheme();

    // Pre-defined metric sets
    coreEventSets = aie::trace::getCoreEventSets(hwGen);
    memoryEventSets = aie::trace::getMemoryEventSets(hwGen);
    memoryTileEventSets = aie::trace::getMemoryTileEventSets(hwGen);
    interfaceTileEventSets = aie::trace::getInterfaceTileEventSets(hwGen);

    // Core/memory module counters
    coreCounterStartEvents = aie::trace::getCoreCounterStartEvents(hwGen, counterScheme);
    coreCounterEndEvents = aie::trace::getCoreCounterEndEvents(hwGen, counterScheme);
    coreCounterEventValues = aie::trace::getCoreCounterEventValues(hwGen, counterScheme);
    memoryCounterStartEvents = aie::trace::getMemoryCounterStartEvents(hwGen, counterScheme);
    memoryCounterEndEvents = aie::trace::getMemoryCounterEndEvents(hwGen, counterScheme);
    memoryCounterEventValues = aie::trace::getMemoryCounterEventValues(hwGen, counterScheme);

    // Core trace start/end: these are also broadcast to memory module
    coreTraceStartEvent = XAIE_EVENT_ACTIVE_CORE;
    coreTraceEndEvent = XAIE_EVENT_USER_EVENT_3_CORE;

    // Memory/interface tile trace is flushed at end of run
    memoryTileTraceStartEvent = XAIE_EVENT_TRUE_MEM_TILE;
    memoryTileTraceEndEvent = XAIE_EVENT_USER_EVENT_1_MEM_TILE;
    interfaceTileTraceStartEvent = XAIE_EVENT_TRUE_PL;
    interfaceTileTraceEndEvent = XAIE_EVENT_USER_EVENT_1_PL;

    // TODO: tranxHandler to record ASM transaction
    // TODO: XAie_cfg to create local aieDevInst
  }

  /****************************************************************************
   * Verify correctness of trace buffer size
   ***************************************************************************/
  uint64_t AieTrace_VE2Impl::checkTraceBufSize(uint64_t aieTraceBufSize)
  {
    uint64_t deviceMemorySize = getPSMemorySize();
    if (deviceMemorySize == 0)
      return aieTraceBufSize;

    double percentSize = (100.0 * aieTraceBufSize) / deviceMemorySize;

    std::stringstream percentSizeStr;
    percentSizeStr << std::fixed << std::setprecision(3) << percentSize;

    // Limit size of trace buffer if requested amount is too high
    if (percentSize >= 80.0) {
      aieTraceBufSize = static_cast<uint64_t>(std::ceil(0.8 * deviceMemorySize));

      std::stringstream newBufSizeStr;
      newBufSizeStr << std::fixed << std::setprecision(3) << (aieTraceBufSize / (1024.0 * 1024.0));  // In MB

      std::string msg = "Requested AIE trace buffer is " + percentSizeStr.str() + "% of device memory." +
                        " You may run into errors depending upon memory usage"
                        " of your application." +
                        " Limiting to " + newBufSizeStr.str() + " MB.";
      xrt_core::message::send(severity_level::warning, "XRT", msg);
    } else {
      std::string msg = "Requested AIE trace buffer is " + percentSizeStr.str() + "% of device memory.";
      xrt_core::message::send(severity_level::info, "XRT", msg);
    }

    return aieTraceBufSize;
  }

  /****************************************************************************
   * Check if given tile has free resources
   ***************************************************************************/
  bool AieTrace_VE2Impl::tileHasFreeRsc(xaiefal::XAieDev* aieDevice, XAie_LocType& loc, 
                                         const module_type type, const std::string& metricSet)
  {
    auto stats = aieDevice->getRscStat(XAIEDEV_DEFAULT_GROUP_AVAIL);
    uint32_t available = 0;
    uint32_t required = 0;
    std::stringstream msg;

    // Check trace events for interface tiles
    if (type == module_type::shim) {
      available = stats.getNumRsc(loc, XAIE_PL_MOD, xaiefal::XAIE_TRACEEVENT);
      required = interfaceTileEventSets[metricSet].size();
      if (available < required) {
        msg << "Available interface tile trace slots for AIE trace : " << available << std::endl
            << "Required interface tile trace slots for AIE trace  : " << required;
        xrt_core::message::send(severity_level::info, "XRT", msg.str());
        return false;
      }
      return true;
    }

    // Memory module/tile perf counters
    available = stats.getNumRsc(loc, XAIE_MEM_MOD, xaiefal::XAIE_PERFCOUNT);
    required = memoryCounterStartEvents.size();
    if (available < required) {
      msg << "Available memory performance counters for AIE trace : " << available << std::endl
          << "Required memory performance counters for AIE trace  : " << required;
      xrt_core::message::send(severity_level::info, "XRT", msg.str());
      return false;
    }

    // Memory module/tile trace slots
    available = stats.getNumRsc(loc, XAIE_MEM_MOD, xaiefal::XAIE_TRACEEVENT);
    required = memoryCounterStartEvents.size() + memoryEventSets[metricSet].size();
    if (available < required) {
      msg << "Available memory trace slots for AIE trace : " << available << std::endl
          << "Required memory trace slots for AIE trace  : " << required;
      xrt_core::message::send(severity_level::info, "XRT", msg.str());
      return false;
    }

    // Core resources not needed in memory tiles
    if (type == module_type::mem_tile)
      return true;

    // Core module perf counters
    available = stats.getNumRsc(loc, XAIE_CORE_MOD, xaiefal::XAIE_PERFCOUNT);
    required = coreCounterStartEvents.size();
    if (metadata->getUseDelay()) {
      ++required;
      if (!metadata->getUseOneDelayCounter())
        ++required;
    } else if (metadata->getUseGraphIterator())
      ++required;

    if (available < required) {
      msg << "Available core module performance counters for AIE trace : " << available << std::endl
          << "Required core module performance counters for AIE trace  : " << required;
      xrt_core::message::send(severity_level::info, "XRT", msg.str());
      return false;
    }

    // Core module trace slots
    available = stats.getNumRsc(loc, XAIE_CORE_MOD, xaiefal::XAIE_TRACEEVENT);
    required = coreCounterStartEvents.size() + coreEventSets[metricSet].size();
    if (available < required) {
      msg << "Available core module trace slots for AIE trace : " << available << std::endl
          << "Required core module trace slots for AIE trace  : " << required;
      xrt_core::message::send(severity_level::info, "XRT", msg.str());
      return false;
    }

    // Core module broadcasts. 2 events for starting/ending trace
    available = stats.getNumRsc(loc, XAIE_CORE_MOD, xaiefal::XAIE_BROADCAST);
    required = memoryEventSets[metricSet].size() + 2;
    if (available < required) {
      msg << "Available core module broadcast channels for AIE trace : " << available << std::endl
          << "Required core module broadcast channels for AIE trace  : " << required;
      xrt_core::message::send(severity_level::info, "XRT", msg.str());
      return false;
    }

    return true;
  }

  /****************************************************************************
   * Stop and release resources (e.g., counters, ports)
   ***************************************************************************/
  void AieTrace_VE2Impl::freeResources()
  {
    for (auto& c : perfCounters) {
      c->stop();
      c->release();
    }
    for (auto& p : streamPorts) {
      p->stop();
      p->release();
    }
  }

  /****************************************************************************
   * Update device (e.g., after loading xclbin)
   ***************************************************************************/
  void AieTrace_VE2Impl::updateDevice()
  {
    // If runtime metrics are not enabled, do not configure trace
    if(!metadata->getRuntimeMetrics())
      return;

    // Set metrics for counters and trace events
    if (!setMetricsSettings(metadata->getDeviceID(), metadata->getHandle())) {
      std::string msg("Unable to configure AIE trace control and events. No trace will be generated.");
      xrt_core::message::send(severity_level::warning, "XRT", msg);
      return;
    }

    // Configure windowed event trace if layer-based start is enabled
    if (xrt_core::config::get_aie_trace_settings_start_type() == "layer") {
      if (!configureWindowedEventTrace(aieDevice)) {
        std::string msg("Unable to configure AIE windowed event trace");
        xrt_core::message::send(severity_level::warning, "XRT", msg);
        return;
      }
    }
  }

  /****************************************************************************
   * Configure windowed event trace for layer-based triggering
   ***************************************************************************/
  bool AieTrace_VE2Impl::configureWindowedEventTrace(xaiefal::XAieDev* aieDevice)
  {
    if (!aieDevice || !aieDevInst) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "AIE device not available for windowed trace configuration");
      return false;
    }

    void* handle = metadata->getHandle();
    boost::property_tree::ptree aiePartitionPt = xdp::aie::getAIEPartitionInfo(handle);

    // Get partition information
    // Column should be relative to the partition, hence startCol is 0.
    uint8_t startCol = 0;
    uint8_t numCols  = static_cast<uint8_t>(aiePartitionPt.back().second.get<uint64_t>("num_cols"));

    auto metadataReader = (VPDatabase::Instance()->getStaticInfo()).getAIEmetadataReader(metadata->getDeviceID());
    if (!metadataReader) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "AIE metadata reader not available for windowed trace configuration");
      return false;
    }

    uint8_t numRows = metadataReader->getNumRows();
    unsigned int startLayer = xrt_core::config::get_aie_trace_settings_start_layer();

    // Reserve broadcast channels using FAL for trace start synchronization
    std::vector<XAie_LocType> vL;
    auto traceStartBroadcastCh1 = aieDevice->broadcast(vL, XAIE_PL_MOD, XAIE_CORE_MOD);
    if (!traceStartBroadcastCh1) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "Unable to create broadcast channel 1 for windowed trace");
      return false;
    }
    if (traceStartBroadcastCh1->reserve() != XAIE_OK) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "Unable to reserve broadcast channel 1 for windowed trace");
      return false;
    }

    auto traceStartBroadcastCh2 = aieDevice->broadcast(vL, XAIE_PL_MOD, XAIE_CORE_MOD);
    if (!traceStartBroadcastCh2) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "Unable to create broadcast channel 2 for windowed trace");
      return false;
    }
    if (traceStartBroadcastCh2->reserve() != XAIE_OK) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "Unable to reserve broadcast channel 2 for windowed trace");
      return false;
    }

    // Get the reserved broadcast channel IDs
    const uint8_t traceStartBroadcastChId1 = traceStartBroadcastCh1->getBc();
    const uint8_t traceStartBroadcastChId2 = traceStartBroadcastCh2->getBc();

    // Reserve and configure performance counter at start column for layer-based triggering
    XAie_Events perfCounterEvent = XAIE_EVENT_PERF_CNT_0_PL;
    if (startLayer != UINT_MAX) {
      auto& shimTile = aieDevice->tile(startCol, 0);
      auto shim = shimTile.pl();

      auto perfCounter = shim.perfCounter();
      if (!perfCounter) {
        xrt_core::message::send(severity_level::warning, "XRT",
            "Unable to create performance counter for windowed trace");
        return false;
      }

      XAie_ModuleType mod = XAIE_PL_MOD;
      if (perfCounter->initialize(mod, XAIE_EVENT_USER_EVENT_0_PL, mod, XAIE_EVENT_USER_EVENT_0_PL) != XAIE_OK) {
        xrt_core::message::send(severity_level::warning, "XRT",
            "Unable to initialize performance counter for windowed trace");
        return false;
      }

      if (perfCounter->reserve() != XAIE_OK) {
        xrt_core::message::send(severity_level::warning, "XRT",
            "Unable to reserve performance counter for windowed trace");
        return false;
      }

      // Get the counter event ID
      perfCounter->getCounterEvent(mod, perfCounterEvent);

      // Set threshold value to startLayer
      perfCounter->changeThreshold(startLayer);

      // Start the performance counter
      if (perfCounter->start() != XAIE_OK) {
        xrt_core::message::send(severity_level::warning, "XRT",
            "Unable to start performance counter for windowed trace");
        return false;
      }
    }


    // Define trace start events for different module types
    XAie_Events shimTraceStartEvent = static_cast<XAie_Events>(XAIE_EVENT_BROADCAST_A_0_PL + traceStartBroadcastChId2);
    XAie_Events memTileTraceStartEvent = static_cast<XAie_Events>(XAIE_EVENT_BROADCAST_0_MEM_TILE + traceStartBroadcastChId1);
    XAie_Events coreModTraceStartEvent = static_cast<XAie_Events>(XAIE_EVENT_BROADCAST_0_CORE + traceStartBroadcastChId1);
    XAie_Events memTraceStartEvent = static_cast<XAie_Events>(XAIE_EVENT_BROADCAST_0_MEM + traceStartBroadcastChId1);

    // Configure trace start events for tiles
    for (auto& tileMetric : metadata->getConfigMetrics()) {
      auto tile = tileMetric.first;
      auto col  = tile.col;
      auto row  = tile.row;
      auto type = aie::getModuleType(row, metadata->getRowOffset());
      auto loc  = XAie_TileLoc(col, row);

      if (startLayer != UINT_MAX) {
        if (type == module_type::shim) {
          // Configure shim/interface tile trace start
          if (col == startCol)
            XAie_TraceStartEvent(aieDevInst, loc, XAIE_PL_MOD, perfCounterEvent);
          else
            XAie_TraceStartEvent(aieDevInst, loc, XAIE_PL_MOD, shimTraceStartEvent);
        }
        else if (type == module_type::mem_tile) {
          // Configure memory tile trace start
          XAie_TraceStartEvent(aieDevInst, loc, XAIE_MEM_MOD, memTileTraceStartEvent);
        }
        else if (type == module_type::core) {
          // Configure core module trace start
          XAie_TraceStartEvent(aieDevInst, loc, XAIE_CORE_MOD, coreModTraceStartEvent);
          XAie_TraceStartEvent(aieDevInst, loc, XAIE_MEM_MOD, memTraceStartEvent);
        }
      }
    }

    // Build 2-channel broadcast network for trace start synchronization
    aie::trace::build2ChannelBroadcastNetwork(aieDevInst, metadata, traceStartBroadcastChId1,
                                               traceStartBroadcastChId2, perfCounterEvent,
                                               startCol, numCols, numRows);

    xrt_core::message::send(severity_level::info, "XRT",
        "Finished AIE windowed trace settings for ve2 using FAL-reserved broadcast channels "
        + std::to_string(traceStartBroadcastChId1) + " and " + std::to_string(traceStartBroadcastChId2));
    return true;
  }

  /****************************************************************************
   * Configure requested tiles with trace metrics and settings
   ***************************************************************************/
  bool AieTrace_VE2Impl::setMetricsSettings(uint64_t deviceId, void* handle)
  {
    if (!metadata->getIsValidMetrics()) {
      std::string msg("AIE trace metrics were not specified in xrt.ini. AIE event trace will not be available.");
      xrt_core::message::send(severity_level::warning, "XRT", msg);
      return false;
    }

    boost::property_tree::ptree aiePartitionPt = xdp::aie::getAIEPartitionInfo(handle);
    // Currently, assuming only one Hw Context is alive at a time
    // Column should be relative to the partition, hence startCol is 0.
    uint8_t startCol = 0;
    uint8_t numCols  = static_cast<uint8_t>(aiePartitionPt.back().second.get<uint64_t>("num_cols"));
    
    // Get channel configurations (memory and interface tiles)
    auto configChannel0 = metadata->getConfigChannel0();
    auto configChannel1 = metadata->getConfigChannel1();

    // Get the column shift for partition
    // NOTE: If partition is not used, this value is zero.
    uint8_t startColShift = metadata->getPartitionOverlayStartCols().front();
    aie::displayColShiftInfo(startColShift);

    // Zero trace event tile counts
    for (int m = 0; m < static_cast<int>(module_type::num_types); ++m) {
      for (int n = 0; n <= NUM_TRACE_EVENTS; ++n)
        mNumTileTraceEvents[m][n] = 0;
    }

    auto metadataReader = (VPDatabase::Instance()->getStaticInfo()).getAIEmetadataReader(deviceId);
    if (!metadataReader) {
      if (aie::isDebugVerbosity()) {
        std::stringstream msg;
        msg << "AIE metadata reader is null";
        xrt_core::message::send(severity_level::debug, "XRT", msg.str());
      }
    }

    auto compilerOptions = metadataReader->getAIECompilerOptions();
    uint8_t numRows = metadataReader->getNumRows();
    int hwGen = metadataReader->getHardwareGeneration();
    std::shared_ptr<xaiefal::XAieBroadcast> traceStartBroadcastCh1 = nullptr, traceStartBroadcastCh2 = nullptr;
    if(compilerOptions.enable_multi_layer) {

      aie::trace::timerSyncronization(aieDevInst,aieDevice, metadata, startCol, numCols, numRows);
      if(xrt_core::config::get_aie_trace_settings_trace_start_broadcast()
         && xrt_core::config::get_aie_trace_settings_start_type() != "layer")
      {
        std::vector<XAie_LocType> vL;
        traceStartBroadcastCh1 = aieDevice->broadcast(vL, XAIE_PL_MOD, XAIE_CORE_MOD);
        traceStartBroadcastCh1->reserve();
        traceStartBroadcastCh2 = aieDevice->broadcast(vL, XAIE_PL_MOD, XAIE_CORE_MOD);
        traceStartBroadcastCh2->reserve();
        aie::trace::build2ChannelBroadcastNetwork(aieDevInst, metadata, traceStartBroadcastCh1->getBc(),
                                                  traceStartBroadcastCh2->getBc(), XAIE_EVENT_COMBO_EVENT_0_PL,
                                                  startCol, numCols, numRows);

        coreTraceStartEvent = (XAie_Events) (XAIE_EVENT_BROADCAST_0_CORE + traceStartBroadcastCh1->getBc());
        memoryTileTraceStartEvent = (XAie_Events) (XAIE_EVENT_BROADCAST_0_MEM_TILE + traceStartBroadcastCh1->getBc());
        interfaceTileTraceStartEvent = (XAie_Events) (XAIE_EVENT_BROADCAST_A_0_PL + traceStartBroadcastCh2->getBc());
      }
    }

    // Using user event for trace end to enable flushing
    // NOTE: Flush trace module always at the end because for some applications
    //       core might be running infinitely.
    
    if (metadata->getUseUserControl())
      coreTraceStartEvent = XAIE_EVENT_INSTR_EVENT_0_CORE;

    // Iterate over all used/specified tiles
    // NOTE: rows are stored as absolute as required by resource manager
    for (auto& tileMetric : metadata->getConfigMetrics()) {
      auto& metricSet = tileMetric.second;
      auto tile       = tileMetric.first;
      auto col        = tile.col + startColShift;
      auto row        = tile.row;
      auto subtype    = tile.subtype;
      auto type       = aie::getModuleType(row, metadata->getRowOffset());
      auto typeInt    = static_cast<int>(type);
      auto& xaieTile  = aieDevice->tile(col, row);
      auto loc        = XAie_TileLoc(col, row);

      if ((type == module_type::core) && !aie::isDmaSet(metricSet)) {
        // If we're not looking at DMA events, then don't display the DMA
        // If core is not active (i.e., DMA-only tile), then ignore this tile
        if (tile.active_core)
          tile.active_memory = false;
        else
          continue;
      }

      std::string tileName = (type == module_type::mem_tile) ? "memory" 
                           : ((type == module_type::shim) ? "interface" : "AIE");
      tileName.append(" tile (" + std::to_string(col) + "," + std::to_string(row) + ")");

      if (aie::isInfoVerbosity()) {
        std::stringstream infoMsg;
        infoMsg << "Configuring " << tileName << " for trace using metric set " << metricSet;
        xrt_core::message::send(severity_level::info, "XRT", infoMsg.str());
      }

      xaiefal::XAieMod core;
      xaiefal::XAieMod memory;
      xaiefal::XAieMod shim;
      if (type == module_type::core)
        core = xaieTile.core();
      if (type == module_type::shim)
        shim = xaieTile.pl();
      else
        memory = xaieTile.mem();

      // Store location to flush at end of run
      if (type == module_type::core || (type == module_type::mem_tile) 
          || (type == module_type::shim)) {
        if (type == module_type::core)
          traceFlushLocs.push_back(loc);
        else if (type == module_type::mem_tile)
          memoryTileTraceFlushLocs.push_back(loc);
        else if (type == module_type::shim)
          interfaceTileTraceFlushLocs.push_back(loc);
      }

      // AIE config object for this tile
      auto cfgTile = std::make_unique<aie_cfg_tile>(col, row, type);
      cfgTile->type = type;
      cfgTile->trace_metric_set = metricSet;
      cfgTile->active_core = tile.active_core;
      cfgTile->active_memory = tile.active_memory;

      // Catch core execution trace
      if ((type == module_type::core) && (metricSet == "execution")) {
        // Set start/end events, use execution packets, and start trace module 
        auto coreTrace = core.traceControl();
        if (coreTrace->setCntrEvent(coreTraceStartEvent, coreTraceEndEvent) != XAIE_OK)
          continue;
        coreTrace->reserve();

        // Driver requires at least one, non-zero trace event
        uint8_t slot;
        coreTrace->reserveTraceSlot(slot);
        coreTrace->setTraceEvent(slot, XAIE_EVENT_TRUE_CORE);

        coreTrace->setMode(XAIE_TRACE_INST_EXEC);
        XAie_Packet pkt = {0, 0};
        coreTrace->setPkt(pkt);
        coreTrace->start();

        (db->getStaticInfo()).addAIECfgTile(deviceId, cfgTile);
        continue;
      }

      // Get vector of pre-defined metrics for this set
      // NOTE: these are local copies as we are adding tile/counter-specific events
      EventVector coreEvents;
      EventVector memoryEvents;
      EventVector interfaceEvents;
      if (type == module_type::core) {
        coreEvents = coreEventSets[metricSet];
        memoryEvents = memoryEventSets[metricSet];
      }
      else if (type == module_type::mem_tile) {
        memoryEvents = memoryTileEventSets[metricSet];
      }
      else if (type == module_type::shim) {
        interfaceEvents = interfaceTileEventSets[metricSet];
      }

      if (coreEvents.empty() && memoryEvents.empty() && interfaceEvents.empty()) {
        std::stringstream msg;
        msg << "Event trace is not available for " << tileName << " using metric set "
            << metricSet << " on hardware generation " << metadata->getHardwareGen() << ".";
        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        continue;
      }

      // Check Resource Availability
      if (!tileHasFreeRsc(aieDevice, loc, type, metricSet)) {
        xrt_core::message::send(severity_level::warning, "XRT",
            "Tile doesn't have enough free resources for trace. Aborting trace configuration.");
        aie::trace::printTileStats(aieDevice, tile);
        return false;
      }

      int numCoreCounters = 0;
      int numMemoryCounters = 0;
      int numCoreTraceEvents = 0;
      int numMemoryTraceEvents = 0;
      int numInterfaceTraceEvents = 0;

      //
      // 1. Reserve and start core module counters (as needed)
      //
      if ((type == module_type::core) && (coreCounterStartEvents.size() > 0)) {
        if (aie::isDebugVerbosity()) {
          std::stringstream msg;
          msg << "Reserving " << coreCounterStartEvents.size() 
              << " core counters for " << tileName;
          xrt_core::message::send(severity_level::debug, "XRT", msg.str());
        }

        XAie_ModuleType mod = XAIE_CORE_MOD;

        for (int i = 0; i < coreCounterStartEvents.size(); ++i) {
          auto perfCounter = core.perfCounter();
          if (perfCounter->initialize(mod, coreCounterStartEvents.at(i), mod, coreCounterEndEvents.at(i)) != XAIE_OK)
            break;
          if (perfCounter->reserve() != XAIE_OK)
            break;

          // NOTE: store events for later use in trace
          XAie_Events counterEvent;
          perfCounter->getCounterEvent(mod, counterEvent);
          int idx = static_cast<int>(counterEvent) - static_cast<int>(XAIE_EVENT_PERF_CNT_0_CORE);
          perfCounter->changeThreshold(coreCounterEventValues.at(i));

          // Set reset event based on counter number
          perfCounter->changeRstEvent(mod, counterEvent);
          coreEvents.push_back(counterEvent);

          // If no memory counters are used, then we need to broadcast the core
          // counter
          if (memoryCounterStartEvents.empty())
            memoryEvents.push_back(counterEvent);

          if (perfCounter->start() != XAIE_OK)
            break;

          perfCounters.push_back(perfCounter);
          numCoreCounters++;

          // Update config file
          uint16_t phyEvent = 0;
          auto& cfg = cfgTile->core_trace_config.pc[idx];
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, coreCounterStartEvents[i], &phyEvent);
          cfg.start_event = phyEvent;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, coreCounterStartEvents[i], &phyEvent);
          cfg.stop_event = phyEvent;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, counterEvent, &phyEvent);
          cfg.reset_event = phyEvent;
          cfg.event_value = coreCounterEventValues[i];
        }
      }

      //
      // 2. Reserve and start memory module counters (as needed)
      //
      if ((type == module_type::core) && (memoryCounterStartEvents.size() > 0)) {
        if (aie::isDebugVerbosity()) {
          std::stringstream msg;
          msg << "Reserving " << memoryCounterStartEvents.size() 
              << " memory counters for " << tileName;
          xrt_core::message::send(severity_level::debug, "XRT", msg.str());
        }

        XAie_ModuleType mod = XAIE_MEM_MOD;

        for (int i = 0; i < memoryCounterStartEvents.size(); ++i) {
          auto perfCounter = memory.perfCounter();
          if (perfCounter->initialize(mod, memoryCounterStartEvents.at(i), mod, memoryCounterEndEvents.at(i)) !=
              XAIE_OK)
            break;
          if (perfCounter->reserve() != XAIE_OK)
            break;

          // Set reset event based on counter number
          XAie_Events counterEvent;
          perfCounter->getCounterEvent(mod, counterEvent);
          int idx = static_cast<int>(counterEvent) - static_cast<int>(XAIE_EVENT_PERF_CNT_0_MEM);
          perfCounter->changeThreshold(memoryCounterEventValues.at(i));

          perfCounter->changeRstEvent(mod, counterEvent);
          memoryEvents.push_back(counterEvent);

          if (perfCounter->start() != XAIE_OK)
            break;

          perfCounters.push_back(perfCounter);
          numMemoryCounters++;

          // Update config file
          uint16_t phyEvent = 0;
          auto& cfg = cfgTile->memory_trace_config.pc[idx];
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, memoryCounterStartEvents[i], &phyEvent);
          cfg.start_event = phyEvent;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, memoryCounterEndEvents[i], &phyEvent);
          cfg.stop_event = phyEvent;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, counterEvent, &phyEvent);
          cfg.reset_event = phyEvent;
          cfg.event_value = memoryCounterEventValues[i];
        }

        // Catch when counters cannot be reserved: report, release, and return
        if ((numCoreCounters < coreCounterStartEvents.size()) ||
            (numMemoryCounters < memoryCounterStartEvents.size())) {
          std::stringstream msg;
          msg << "Unable to reserve " << coreCounterStartEvents.size() 
              << " core counters and " << memoryCounterStartEvents.size() 
              << " memory counters for " << tileName << " required for trace.";
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());

          freeResources();
          // Print resources availability for this tile
          aie::trace::printTileStats(aieDevice, tile);
          return false;
        }
      }

      //
      // 3. Configure Core Tracing Events
      //
      if (type == module_type::core) {
        if (aie::isDebugVerbosity()) {
          std::stringstream msg;
          msg << "Reserving " << coreEvents.size() << " core trace events for " << tileName;
          xrt_core::message::send(severity_level::debug, "XRT", msg.str());
        }

        XAie_ModuleType mod = XAIE_CORE_MOD;
        uint16_t phyEvent = 0;
        auto coreTrace = core.traceControl();

        // Delay cycles and user control are not compatible with each other
        if (metadata->getUseGraphIterator()) {
          if (!aie::trace::configStartIteration(core, metadata->getIterationCount(), 
                                                coreTraceStartEvent))
            break;
        } else if (metadata->getUseDelay()) {
          if (!aie::trace::configStartDelay(core, metadata->getDelay(), 
                                            coreTraceStartEvent))
            break;
        }

        // Configure combo & group events (e.g., to monitor DMA channels)
        auto comboEvents = aie::trace::configComboEvents(aieDevInst, xaieTile, loc, mod, type, 
                                                         metricSet, cfgTile->core_trace_config);
        aie::trace::configGroupEvents(aieDevInst, loc, mod, type, metricSet);

        // Set overall start/end for trace capture
        if (coreTrace->setCntrEvent(coreTraceStartEvent, coreTraceEndEvent) != XAIE_OK)
          break;

        auto ret = coreTrace->reserve();
        if (ret != XAIE_OK) {
          std::stringstream msg;
          msg << "Unable to reserve core module trace control for " << tileName;
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());

          freeResources();
          // Print resources availability for this tile
          aie::trace::printTileStats(aieDevice, tile);
          return false;
        }

        for (int i = 0; i < coreEvents.size(); i++) {
          uint8_t slot;
          if (coreTrace->reserveTraceSlot(slot) != XAIE_OK)
            break;
          if (coreTrace->setTraceEvent(slot, coreEvents[i]) != XAIE_OK)
            break;
          numCoreTraceEvents++;

          // Update config file
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, coreEvents[i], &phyEvent);
          cfgTile->core_trace_config.traced_events[slot] = phyEvent;
        }

        // Update config file
        XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, coreTraceStartEvent, &phyEvent);
        cfgTile->core_trace_config.start_event = phyEvent;
        XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, coreTraceEndEvent, &phyEvent);
        cfgTile->core_trace_config.stop_event = phyEvent;

        // Record allocated trace events
        mNumTileTraceEvents[typeInt][numCoreTraceEvents]++;
        coreEvents.clear();

        // Specify packet type and ID then start core trace
        // NOTE: always use PC packets
        if (coreTrace->setMode(XAIE_TRACE_EVENT_PC) != XAIE_OK)
          break;
        XAie_Packet pkt = {0, 0};
        if (coreTrace->setPkt(pkt) != XAIE_OK)
          break;
        if (coreTrace->start() != XAIE_OK)
          break;
      }

      //
      // 4. Configure Memory Tracing Events
      //
      // NOTE: this is applicable for memory modules in AIE tiles or memory tiles
      uint32_t coreToMemBcMask = 0;
      if ((type == module_type::core) || (type == module_type::mem_tile)) {
        if (aie::isDebugVerbosity()) {
          xrt_core::message::send(severity_level::debug, "XRT", "Reserving " +
            std::to_string(memoryEvents.size()) + " memory trace events for " + tileName);
        }

        // Set overall start/end for trace capture
        // NOTE: this should be done first for FAL-based implementations
        auto memoryTrace = memory.traceControl();
        auto traceStartEvent = (type == module_type::core) ? coreTraceStartEvent : memoryTileTraceStartEvent;
        auto traceEndEvent = (type == module_type::core) ? coreTraceEndEvent : memoryTileTraceEndEvent;
        
        aie_cfg_base& aieConfig = cfgTile->core_trace_config;
        if (type == module_type::mem_tile)
          aieConfig = cfgTile->memory_tile_trace_config;

        // Configure combo events for metric sets that include DMA events        
        auto comboEvents = aie::trace::configComboEvents(aieDevInst, xaieTile, loc, 
            XAIE_MEM_MOD, type, metricSet, aieConfig);
        if (comboEvents.size() == 2) {
          traceStartEvent = comboEvents.at(0);
          traceEndEvent = comboEvents.at(1);
        }

        if(compilerOptions.enable_multi_layer && type == module_type::core
          && xrt_core::config::get_aie_trace_settings_trace_start_broadcast()
          && xrt_core::config::get_aie_trace_settings_start_type() != "layer")
        {
          traceStartEvent = (XAie_Events) (XAIE_EVENT_BROADCAST_0_MEM + traceStartBroadcastCh1->getBc());
        }
        
        // Configure event ports on stream switch
        // NOTE: These are events from the core module stream switch
        //       outputted on the memory module trace stream. 
        streamPorts = aie::trace::configStreamSwitchPorts(aieDevInst, tile,
            xaieTile, loc, type, metricSet, 0, 0, memoryEvents, aieConfig);
          
        // Set overall start/end for trace capture
        if (memoryTrace->setCntrEvent(traceStartEvent, traceEndEvent) != XAIE_OK)
          break;

        auto ret = memoryTrace->reserve();
        if (ret != XAIE_OK) {
          std::stringstream msg;
          msg << "Unable to reserve memory trace control for " << tileName;
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());

          freeResources();
          // Print resources availability for this tile
          aie::trace::printTileStats(aieDevice, tile);
          return false;
        }

        // Specify Sel0/Sel1 for memory tile events 21-44
        if (type == module_type::mem_tile) {
          auto iter0 = configChannel0.find(tile);
          auto iter1 = configChannel1.find(tile);
          uint8_t channel0 = (iter0 == configChannel0.end()) ? 0 : iter0->second;
          uint8_t channel1 = (iter1 == configChannel1.end()) ? 1 : iter1->second;
          aie::trace::configEventSelections(aieDevInst, tile, loc, type, metricSet, channel0, 
                                            channel1, cfgTile->memory_tile_trace_config);
        }
        else {
          // Record if these are channel-specific events
          // NOTE: for now, check first event and assume single channel
          auto channelNum = aie::getChannelNumberFromEvent(memoryEvents.at(0));
          if (channelNum >= 0) {
            if (aie::isInputSet(type, metricSet)) {
              cfgTile->core_trace_config.mm2s_channels[0] = channelNum;
              if (channelNum < tile.mm2s_names.size())
                cfgTile->core_trace_config.mm2s_names[0] = tile.mm2s_names.at(channelNum);
            }
            else {
              cfgTile->core_trace_config.s2mm_channels[0] = channelNum;
              if (channelNum < tile.s2mm_names.size())
                cfgTile->core_trace_config.s2mm_names[0] = tile.s2mm_names.at(channelNum);
            }
          }
        }

        // Configure memory trace events
        for (int i = 0; i < memoryEvents.size(); i++) {
          bool isCoreEvent = aie::isCoreModuleEvent(memoryEvents[i]);
          XAie_ModuleType mod = isCoreEvent ? XAIE_CORE_MOD : XAIE_MEM_MOD;

          auto TraceE = memory.traceEvent();
          TraceE->setEvent(mod, memoryEvents[i]);
          if (TraceE->reserve() != XAIE_OK)
            break;
          if (TraceE->start() != XAIE_OK)
            break;
          numMemoryTraceEvents++;
          
          // Configure edge events (as needed)
          aie::trace::configEdgeEvents(aieDevInst, tile, type, metricSet, memoryEvents[i]);

          // Update config file
          // Get Trace slot
          uint32_t S = 0;
          XAie_LocType L;
          XAie_ModuleType M;
          TraceE->getRscId(L, M, S);

          // Get physical event
          uint16_t phyEvent = 0;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, memoryEvents[i], &phyEvent);

          if (isCoreEvent) {
            auto bcId = TraceE->getBc();
            coreToMemBcMask |= (1 << bcId);
            
            cfgTile->core_trace_config.internal_events_broadcast[bcId] = phyEvent;
            cfgTile->memory_trace_config.traced_events[S] = aie::bcIdToEvent(bcId);
          }
          else if (type == xdp::module_type::mem_tile)
            cfgTile->memory_tile_trace_config.traced_events[S] = phyEvent;
          else
            cfgTile->memory_trace_config.traced_events[S] = phyEvent;
        }

        // Add trace control events to config file
        {
          uint16_t phyEvent = 0;

          // Start
          if (aie::isCoreModuleEvent(traceStartEvent)) {
            auto bcId = memoryTrace->getStartBc();
            coreToMemBcMask |= (1 << bcId);

            XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_CORE_MOD, traceStartEvent, &phyEvent);
            cfgTile->core_trace_config.internal_events_broadcast[bcId] = phyEvent;
            cfgTile->memory_trace_config.start_event = aie::bcIdToEvent(bcId);
          }
          else {
            XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_MEM_MOD, traceStartEvent, &phyEvent);
            if (type == module_type::mem_tile)
              cfgTile->memory_tile_trace_config.start_event = phyEvent;
            else
              cfgTile->memory_trace_config.start_event = phyEvent;
          }

          // Stop
          if (aie::isCoreModuleEvent(traceEndEvent)) {
            auto bcId = memoryTrace->getStopBc();
            coreToMemBcMask |= (1 << bcId);
          
            XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_CORE_MOD, traceEndEvent, &phyEvent);
            cfgTile->core_trace_config.internal_events_broadcast[bcId] = phyEvent;
            cfgTile->memory_trace_config.stop_event = aie::bcIdToEvent(bcId);

            // Use east broadcasting for AIE2+ or odd absolute rows of AIE1 checkerboard
            if ((row % 2) || (metadata->getHardwareGen() > 1))
              cfgTile->core_trace_config.broadcast_mask_east = coreToMemBcMask;
            else
              cfgTile->core_trace_config.broadcast_mask_west = coreToMemBcMask;
          }
          else {
            XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_MEM_MOD, traceEndEvent, &phyEvent);
            if (type == module_type::mem_tile)
              cfgTile->memory_tile_trace_config.stop_event = phyEvent;
            else
              cfgTile->memory_trace_config.stop_event = phyEvent;
          }
        }

        // Record allocated trace events
        mNumTileTraceEvents[typeInt][numMemoryTraceEvents]++;
        memoryEvents.clear();
        
        // Specify packet type and ID then start memory trace
        // NOTE: always use time packets
        if (memoryTrace->setMode(XAIE_TRACE_EVENT_TIME) != XAIE_OK)
          break;
        uint8_t packetType = (type == module_type::mem_tile) ? 3 : 1;
        XAie_Packet pkt = {0, packetType};
        if (memoryTrace->setPkt(pkt) != XAIE_OK)
          break;
        if (memoryTrace->start() != XAIE_OK)
          break;

        // Update memory packet type in config file
        if (type == module_type::mem_tile)
          cfgTile->memory_tile_trace_config.packet_type = packetType;
        else
          cfgTile->memory_trace_config.packet_type = packetType;
      }

      //
      // 5. Configure Interface Tile Tracing Events
      //
      if (type == module_type::shim) {
        if (aie::isDebugVerbosity()) {
          std::stringstream msg;
          msg << "Reserving " << interfaceEvents.size() << " trace events for " << tileName;
          xrt_core::message::send(severity_level::debug, "XRT", msg.str());
        }

        auto shimTrace = shim.traceControl();

	if(col == startCol && compilerOptions.enable_multi_layer
           && xrt_core::config::get_aie_trace_settings_trace_start_broadcast()
           && xrt_core::config::get_aie_trace_settings_start_type() != "layer")
        {
          if (shimTrace->setCntrEvent(XAIE_EVENT_COMBO_EVENT_0_PL, interfaceTileTraceEndEvent) != XAIE_OK)
            break;
        }
        else
        {
          if (shimTrace->setCntrEvent(interfaceTileTraceStartEvent, interfaceTileTraceEndEvent) != XAIE_OK)
            break;
        }

        auto ret = shimTrace->reserve();
        if (ret != XAIE_OK) {
          std::stringstream msg;
          msg << "Unable to reserve trace control for " << tileName;
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());

          freeResources();
          // Print resources availability for this tile
          aie::trace::printTileStats(aieDevice, tile);
          return false;
        }

        // Specify channels for interface tile DMA events
        auto iter0 = configChannel0.find(tile);
        auto iter1 = configChannel1.find(tile);
        uint8_t channel0 = (iter0 == configChannel0.end()) ? 0 : iter0->second;
        uint8_t channel1 = (iter1 == configChannel1.end()) ? 1 : iter1->second;

        // Modify events as needed
        aie::trace::modifyEvents(type, subtype, metricSet, channel0, interfaceEvents, hwGen);

        streamPorts = aie::trace::configStreamSwitchPorts(aieDevInst, tile, xaieTile, loc, type, metricSet, 
                                                          channel0, channel1, interfaceEvents, 
                                                          cfgTile->interface_tile_trace_config);

        // Configure interface tile trace events
        for (int i = 0; i < interfaceEvents.size(); i++) {
          auto event = interfaceEvents.at(i);
          auto TraceE = shim.traceEvent();
          TraceE->setEvent(XAIE_PL_MOD, event);
          if (TraceE->reserve() != XAIE_OK)
            break;
          if (TraceE->start() != XAIE_OK)
            break;
          numInterfaceTraceEvents++;

          // Update config file
          // Get Trace slot
          uint32_t S = 0;
          XAie_LocType L;
          XAie_ModuleType M;
          TraceE->getRscId(L, M, S);
          // Get Physical event
          uint16_t phyEvent = 0;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_PL_MOD, event, &phyEvent);
          cfgTile->interface_tile_trace_config.traced_events[S] = phyEvent;
        }

        // Update config file
        {
          // Add interface trace control events
          // Start
          uint16_t phyEvent = 0;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_PL_MOD, interfaceTileTraceStartEvent, &phyEvent);
          cfgTile->interface_tile_trace_config.start_event = phyEvent;
          // Stop
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_PL_MOD, interfaceTileTraceEndEvent, &phyEvent);
          cfgTile->interface_tile_trace_config.stop_event = phyEvent;
        }

        // Record allocated trace events
        mNumTileTraceEvents[typeInt][numInterfaceTraceEvents]++;
        
        // Specify packet type and ID then start interface tile trace
        // NOTE: always use time packets
        if (shimTrace->setMode(XAIE_TRACE_EVENT_TIME) != XAIE_OK)
          break;
        uint8_t packetType = 4;
        XAie_Packet pkt = {0, packetType};
        if (shimTrace->setPkt(pkt) != XAIE_OK)
          break;
        if (shimTrace->start() != XAIE_OK)
          break;
        cfgTile->interface_tile_trace_config.packet_type = packetType;
        auto channelNum = aie::getChannelNumberFromEvent(interfaceEvents.at(0));
        if (channelNum >= 0) {
          if (aie::isInputSet(type, metricSet)) {
            cfgTile->interface_tile_trace_config.mm2s_channels[channelNum] = channelNum;
            if (channelNum < tile.mm2s_names.size())
              cfgTile->interface_tile_trace_config.mm2s_names[channelNum] = tile.mm2s_names.at(channelNum);
          }
          else {
            cfgTile->interface_tile_trace_config.s2mm_channels[channelNum] = channelNum;
            if (channelNum < tile.s2mm_names.size())
              cfgTile->interface_tile_trace_config.s2mm_names[channelNum] = tile.s2mm_names.at(channelNum);
          }
        }
      } // interface tiles

      if (aie::isDebugVerbosity()) {
        std::stringstream msg;
        msg << "Reserved ";
        if (type == module_type::core)
          msg << numCoreTraceEvents << " core and " << numMemoryTraceEvents << " memory";
        else if (type == module_type::mem_tile)
          msg << numMemoryTraceEvents << " memory tile";
        else if (type == module_type::shim)
          msg << numInterfaceTraceEvents << " interface tile";
        msg << " trace events for " << tileName << ". Adding tile to static database.";
        xrt_core::message::send(severity_level::debug, "XRT", msg.str());
      }

      // Add config info to static database
      // NOTE: Do not access cfgTile after this
      (db->getStaticInfo()).addAIECfgTile(deviceId, cfgTile);
    }  // For tiles

    // Report and store trace events per tile
    for (int m = 0; m < static_cast<int>(module_type::num_types); ++m) {
      aie::trace::printTraceEventStats(m, mNumTileTraceEvents[m]);
      for (int n = 0; n <= NUM_TRACE_EVENTS; ++n)
        (db->getStaticInfo()).addAIECoreEventResources(deviceId, n, mNumTileTraceEvents[m][n]);
    }
    return true;
  }  // end setMetricsSettings

  /****************************************************************************
   * Flush trace modules by forcing end events
   *
   * Trace modules buffer partial packets. At end of run, this needs to be 
   * flushed using a custom end event. This applies to trace windowing and 
   * passive tiles like memory and interface.
   *
   ***************************************************************************/
  void AieTrace_VE2Impl::flushTraceModules()
  {
    if (traceFlushLocs.empty() && memoryTileTraceFlushLocs.empty()
        && interfaceTileTraceFlushLocs.empty())
      return;

    if(aieDevInst == nullptr)
    {
      std::stringstream msg;
      msg << "AIE device instance is not available. AIE Trace might be empty/incomplete as "
          << "flushing won't be performed.";
      xrt_core::message::send(severity_level::debug, "XRT", msg.str());
      return;
    }

    if (aie::isDebugVerbosity()) {
      std::stringstream msg;
      msg << "Flushing AIE trace by forcing end event for " << traceFlushLocs.size()
          << " AIE tiles, " << memoryTileTraceFlushLocs.size() << " memory tiles, and " 
          << interfaceTileTraceFlushLocs.size() << " interface tiles.";
      xrt_core::message::send(severity_level::debug, "XRT", msg.str());
    }

    // Flush trace by forcing end event
    // NOTE: this informs tiles to output remaining packets (even if partial)
    for (const auto& loc : traceFlushLocs) 
      XAie_EventGenerate(aieDevInst, loc, XAIE_CORE_MOD, coreTraceEndEvent);
    for (const auto& loc : memoryTileTraceFlushLocs)
      XAie_EventGenerate(aieDevInst, loc, XAIE_MEM_MOD, memoryTileTraceEndEvent);
    for (const auto& loc : interfaceTileTraceFlushLocs)
      XAie_EventGenerate(aieDevInst, loc, XAIE_PL_MOD, interfaceTileTraceEndEvent);

    traceFlushLocs.clear();
    memoryTileTraceFlushLocs.clear();
    interfaceTileTraceFlushLocs.clear();
  }

  /****************************************************************************
   * Poll AIE timers (for system timeline only)
   ***************************************************************************/
  void AieTrace_VE2Impl::pollTimers(uint64_t index, void* handle)
  {
    // Wait until xclbin has been loaded and device has been updated in database
    if (!(db->getStaticInfo().isDeviceReady(index)))
      return;

    // Only read first timer and assume common time domain across all tiles
    static auto tileMetrics = metadata->getConfigMetrics();
    if (tileMetrics.empty())
      return;

    static auto tile   = tileMetrics.begin()->first;
    auto loc           = XAie_TileLoc(tile.col, tile.row);
    auto moduleType    = aie::getModuleType(tile.row, metadata->getRowOffset());
    auto falModuleType =  (moduleType == module_type::core) ? XAIE_CORE_MOD 
                       : ((moduleType == module_type::shim) ? XAIE_PL_MOD 
                       : XAIE_MEM_MOD);

    uint64_t timerValue = 0;  
    auto timestamp1 = xrt_core::time_ns();
    XAie_ReadTimer(aieDevInst, loc, falModuleType, &timerValue);
    auto timestamp2 = xrt_core::time_ns();
    
    std::vector<uint64_t> values;
    values.push_back(tile.col);
    values.push_back( aie::getRelativeRow(tile.row, metadata->getRowOffset()) );
    values.push_back(timerValue);

    db->getDynamicInfo().addAIETimerSample(index, timestamp1, timestamp2, values);
  }

  /****************************************************************************
   * Set AIE device instance
   ***************************************************************************/
  void* AieTrace_VE2Impl::setAieDeviceInst(void* handle, uint64_t deviceID)
  {
    aieDevInst = static_cast<XAie_DevInst*>(db->getStaticInfo().getAieDevInst(fetchAieDevInst, handle, deviceID));
    aieDevice = static_cast<xaiefal::XAieDev*>(db->getStaticInfo().getAieDevice(allocateAieDevice, deallocateAieDevice, handle, deviceID));
    return aieDevInst;
  }
}  // namespace xdp

#else // XDNA flow

namespace xdp {
  using severity_level = xrt_core::message::severity_level;

  /****************************************************************************
   * Constructor: AIE trace implementation for edge devices
   ***************************************************************************/
  AieTrace_VE2Impl::AieTrace_VE2Impl(VPDatabase* database, std::shared_ptr<AieTraceMetadata> metadata)
      : AieTraceImpl(database, metadata)
  {
    auto hwGen = metadata->getHardwareGen();
    
    // Pre-defined metric sets
    coreEventSets = aie::trace::getCoreEventSets(hwGen);
    memoryEventSets = aie::trace::getMemoryEventSets(hwGen);
    memoryTileEventSets = aie::trace::getMemoryTileEventSets(hwGen);
    interfaceTileEventSets = aie::trace::getInterfaceTileEventSets(hwGen);

    // Core trace start/end: these are also broadcast to memory module
    coreTraceStartEvent = XAIE_EVENT_ACTIVE_CORE;
    coreTraceEndEvent = XAIE_EVENT_USER_EVENT_3_CORE;

    // Memory/interface tile trace is flushed at end of run
    memoryTileTraceStartEvent = XAIE_EVENT_TRUE_MEM_TILE;
    memoryTileTraceEndEvent = XAIE_EVENT_USER_EVENT_1_MEM_TILE;
    interfaceTileTraceStartEvent = XAIE_EVENT_TRUE_PL;
    interfaceTileTraceEndEvent = XAIE_EVENT_USER_EVENT_1_PL;

    tranxHandler = std::make_unique<aie::VE2Transaction>();

    xdp::aie::driver_config meta_config = metadata->getAIEConfigMetadata();
    XAie_Config cfg {
      meta_config.hw_gen,
      meta_config.base_address,
      meta_config.column_shift,
      meta_config.row_shift,
      meta_config.num_rows,
      meta_config.num_columns,
      meta_config.shim_row,
      meta_config.mem_row_start,
      meta_config.mem_num_rows,
      meta_config.aie_tile_row_start,
      meta_config.aie_tile_num_rows,
      {0} // PartProp
    };

    auto RC = XAie_CfgInitialize(&aieDevInst, &cfg);
    if (RC != XAIE_OK)
      xrt_core::message::send(severity_level::warning, "XRT", "AIE Driver Initialization Failed.");
  }

  /****************************************************************************
   * Verify correctness of trace buffer size
   ***************************************************************************/
  uint64_t AieTrace_VE2Impl::checkTraceBufSize(uint64_t aieTraceBufSize)
  {
    uint64_t deviceMemorySize = getPSMemorySize();
    if (deviceMemorySize == 0)
      return aieTraceBufSize;

    double percentSize = (100.0 * aieTraceBufSize) / deviceMemorySize;

    std::stringstream percentSizeStr;
    percentSizeStr << std::fixed << std::setprecision(3) << percentSize;

    // Limit size of trace buffer if requested amount is too high
    if (percentSize >= 80.0) {
      aieTraceBufSize = static_cast<uint64_t>(std::ceil(0.8 * deviceMemorySize));

      std::stringstream newBufSizeStr;
      newBufSizeStr << std::fixed << std::setprecision(3) << (aieTraceBufSize / (1024.0 * 1024.0));  // In MB

      std::string msg = "Requested AIE trace buffer is " + percentSizeStr.str() + "% of device memory." +
                        " You may run into errors depending upon memory usage"
                        " of your application." +
                        " Limiting to " + newBufSizeStr.str() + " MB.";
      xrt_core::message::send(severity_level::warning, "XRT", msg);
    } else {
      std::string msg = "Requested AIE trace buffer is " + percentSizeStr.str() + "% of device memory.";
      xrt_core::message::send(severity_level::info, "XRT", msg);
    }

    return aieTraceBufSize;
  }

  /****************************************************************************
   * Update device (e.g., after loading xclbin)
   ***************************************************************************/
  void AieTrace_VE2Impl::updateDevice()
  {
    xrt_core::message::send(severity_level::info, "XRT", "Calling AIE Trace VE2 XDNA updateDevice.");

    // If runtime metrics are not enabled, do not configure trace
    if(!metadata->getRuntimeMetrics())
      return;

      boost::property_tree::ptree aiePartitionPt = xdp::aie::getAIEPartitionInfo(metadata->getHandle());
      if (aiePartitionPt.empty()) {
        xrt_core::message::send(severity_level::warning, "XRT",
                                "AIE trace: no partition info for trace-start broadcast; skipping broadcast network.");
        return;
      }
      uint8_t startCol = static_cast<uint8_t>(aiePartitionPt.back().second.get<uint64_t>("start_col"));
      uint8_t numCols = static_cast<uint8_t>(aiePartitionPt.back().second.get<uint64_t>("num_cols")); // !!! should be 36

      std::cout << "!!! updateDevice startCol (back): " << static_cast<int>(startCol) << std::endl;
      std::cout << "!!! updateDevice numCols (back): " << static_cast<int>(numCols) << std::endl;

      startCol = static_cast<uint8_t>(aiePartitionPt.front().second.get<uint64_t>("start_col"));
      numCols = static_cast<uint8_t>(aiePartitionPt.front().second.get<uint64_t>("num_cols")); // !!! should be 36

      std::cout << "!!! updateDevice startCol (front): " << static_cast<int>(startCol) << std::endl;
      std::cout << "!!! updateDevice numCols (front): " << static_cast<int>(numCols) << std::endl; 

    // Set metrics for counters and trace events
    if (!setMetricsSettings(metadata->getDeviceID(), metadata->getHandle())) {
      std::string msg("Unable to configure AIE trace control and events. No trace will be generated.");
      xrt_core::message::send(severity_level::warning, "XRT", msg);
      return;
    }

    // Configure windowed event trace if layer-based start is enabled
    if (xrt_core::config::get_aie_trace_settings_start_type() == "layer") {
      if (!configureWindowedEventTrace(metadata->getHandle())) {
        std::string msg("Unable to configure AIE windowed event trace");
        xrt_core::message::send(severity_level::warning, "XRT", msg);
        return;
      }
    }
  }

  /****************************************************************************
   * Configure windowed event trace for layer-based triggering
   ***************************************************************************/
  bool AieTrace_VE2Impl::configureWindowedEventTrace(void* hwCtxImpl)
  {
    // Start recording the windowed event trace transaction
    if (!tranxHandler->initializeTransaction(&aieDevInst, "AieTraceWindow")) {
      xrt_core::message::send(severity_level::warning, "XRT", "AIE TRACE: Failed to initialize transaction for Windowed Event Trace.");
      return false;
    }

    boost::property_tree::ptree aiePartitionPt = xdp::aie::getAIEPartitionInfo(hwCtxImpl);
    // Currently, assuming only one Hw Context is alive at a time
    //uint8_t startCol = static_cast<uint8_t>(aiePartitionPt.back().second.get<uint64_t>("start_col"));
    uint8_t startCol = 0;
    uint8_t numCols  = static_cast<uint8_t>(aiePartitionPt.back().second.get<uint64_t>("num_cols"));

    std::cout << "!!! startCol: " << startCol << std::endl;
    std::cout << "!!! numCols: " << numCols << std::endl;

    auto metadataReader = (VPDatabase::Instance()->getStaticInfo()).getAIEmetadataReader(metadata->getDeviceID());
    if (!metadataReader) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "AIE metadata reader not available for windowed trace configuration");
      return false;
    }
    
    // XDNA path has no xaiefal broadcast reservation; use fixed channels (see
    // client/resources_def.h traceStartBroadcastChId1 / traceStartBroadcastChId2).
    const uint8_t traceStartBroadcastChId1 = 6;
    const uint8_t traceStartBroadcastChId2 = 7;

    // Define trace start events for different module types
    XAie_Events shimTraceStartEvent = (XAie_Events) (XAIE_EVENT_BROADCAST_A_0_PL + traceStartBroadcastChId2);
    XAie_Events memTileTraceStartEvent = (XAie_Events)(XAIE_EVENT_BROADCAST_0_MEM_TILE + traceStartBroadcastChId1);
    XAie_Events coreModTraceStartEvent = (XAie_Events)(XAIE_EVENT_BROADCAST_0_CORE + traceStartBroadcastChId1);
    XAie_Events memTraceStartEvent = (XAie_Events)(XAIE_EVENT_BROADCAST_0_MEM + traceStartBroadcastChId1);
        
    unsigned int startLayer = xrt_core::config::get_aie_trace_settings_start_layer();

    // Configure trace start events for tiles
    // NOTE: rows are stored as absolute as required by resource manager
    for (auto& tileMetric : metadata->getConfigMetrics()) {
      auto tile = tileMetric.first;
      auto col  = tile.col;
      auto row  = tile.row;
      auto type = aie::getModuleType(row, metadata->getRowOffset());
      auto loc  = XAie_TileLoc(col, row);

      if (startLayer != UINT_MAX) {
        if (type == module_type::shim) {
          // Configure shim/interface tile trace start
          if (col == startCol)
            XAie_TraceStartEvent(&aieDevInst, loc, XAIE_PL_MOD, XAIE_EVENT_PERF_CNT_0_PL);
          else
            XAie_TraceStartEvent(&aieDevInst, loc, XAIE_PL_MOD, shimTraceStartEvent);
        }
        else if (type == module_type::mem_tile) {
          // Configure memory tile trace start
          XAie_TraceStartEvent(&aieDevInst, loc, XAIE_MEM_MOD, memTileTraceStartEvent);
        }
        else if (type == module_type::core) {
          // Configure core module trace start
          XAie_TraceStartEvent(&aieDevInst, loc, XAIE_CORE_MOD, coreModTraceStartEvent);
          XAie_TraceStartEvent(&aieDevInst, loc, XAIE_MEM_MOD, memTraceStartEvent);
        }
      }
    }

    if (startLayer != UINT_MAX) {
        XAie_PerfCounterControlSet(&aieDevInst, XAie_TileLoc(startCol, 0), XAIE_PL_MOD, 0, XAIE_EVENT_USER_EVENT_0_PL, XAIE_EVENT_USER_EVENT_0_PL);
        XAie_PerfCounterEventValueSet(&aieDevInst, XAie_TileLoc(startCol, 0), XAIE_PL_MOD, 0, startLayer);
    }

    // Build 2-channel broadcast network for trace start synchronization
    build2ChannelBroadcastNetwork(hwCtxImpl, traceStartBroadcastChId1, traceStartBroadcastChId2, XAIE_EVENT_PERF_CNT_0_PL);

    xrt_core::message::send(severity_level::info, "XRT", "Finished AIE Windowed Trace Settings.");
    auto hwContext = metadata->getHwContext();
      
    // Submit the windowed event trace transaction
    if (!tranxHandler->submitTransaction(&aieDevInst, hwContext)) {
      xrt_core::message::send(severity_level::warning, "XRT", "AIE TRACE: Failed to submit windowed event trace transaction.");
      return false;
    }

    return true;
  }

  /****************************************************************************
   * Configure requested tiles with trace metrics and settings
   ***************************************************************************/
  bool AieTrace_VE2Impl::setMetricsSettings(uint64_t deviceId, void* handle)
  {
    if (!metadata->getIsValidMetrics()) {
      std::string msg("AIE trace metrics were not specified in xrt.ini. AIE event trace will not be available.");
      xrt_core::message::send(severity_level::warning, "XRT", msg);
      return false;
    }

    // Get partition columns
    boost::property_tree::ptree aiePartitionPt = xdp::aie::getAIEPartitionInfo(handle);
    // Currently, assuming only one Hw Context is alive at a time
    //uint8_t startCol = static_cast<uint8_t>(aiePartitionPt.front().second.get<uint64_t>("start_col"));
    uint8_t startCol = 0;
    uint8_t numCols  = static_cast<uint8_t>(aiePartitionPt.back().second.get<uint64_t>("num_cols"));

    std::string startType = xrt_core::config::get_aie_trace_settings_start_type();
    unsigned int startLayer = xrt_core::config::get_aie_trace_settings_start_layer();

    tranxHandler->clearExternalAsmOverride();

    std::string tranxName = "AieTraceMetrics";
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
        "Starting transaction " + tranxName);
    if (!tranxHandler->initializeTransaction(&aieDevInst, tranxName)) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "AIE TRACE: Failed to initialize transaction for AIE TRACE Metrics.");
      return false;
    }

    // XDNA: fixed broadcast IDs for trace-start network (matches client/resources_def.h).
    const uint8_t traceStartBroadcastChId1 = 6;
    const uint8_t traceStartBroadcastChId2 = 7;
    
    // Get channel configurations (memory and interface tiles)
    auto configChannel0 = metadata->getConfigChannel0();
    auto configChannel1 = metadata->getConfigChannel1();

    // Get the column shift for partition
    // NOTE: If partition is not used, this value is zero.
    uint8_t startColShift = metadata->getPartitionOverlayStartCols().front();
    aie::displayColShiftInfo(startColShift);

    // Zero trace event tile counts
    for (int m = 0; m < static_cast<int>(module_type::num_types); ++m) {
      for (int n = 0; n <= NUM_TRACE_EVENTS; ++n)
        mNumTileTraceEvents[m][n] = 0;
    }

    auto metadataReader = (VPDatabase::Instance()->getStaticInfo()).getAIEmetadataReader(deviceId);
    if (!metadataReader) {
      if (aie::isDebugVerbosity()) {
        std::stringstream msg;
        msg << "AIE metadata reader is null";
        xrt_core::message::send(severity_level::debug, "XRT", msg.str());
      }
    }

    // Using user event for trace end to enable flushing
    // NOTE: Flush trace module always at the end because for some applications
    //       core might be running infinitely.
    if (metadata->getUseUserControl())
      coreTraceStartEvent = XAIE_EVENT_INSTR_EVENT_0_CORE;
    coreTraceEndEvent = XAIE_EVENT_USER_EVENT_3_CORE;

    // Iterate over all used/specified tiles
    // NOTE: rows are stored as absolute as required by resource manager
    for (auto& tileMetric : metadata->getConfigMetrics()) {
      auto& metricSet = tileMetric.second;
      auto tile       = tileMetric.first;
      auto col        = tile.col + startColShift;
      auto row        = tile.row;
      auto subtype    = tile.subtype;
      auto type       = aie::getModuleType(row, metadata->getRowOffset());
      auto typeInt    = static_cast<int>(type);
      auto loc        = XAie_TileLoc(col, row);

      if ((type == module_type::core) && !aie::isDmaSet(metricSet)) {
        // If we're not looking at DMA events, then don't display the DMA
        // If core is not active (i.e., DMA-only tile), then ignore this tile
        if (tile.active_core)
          tile.active_memory = false;
        else
          continue;
      }

      std::string tileName = (type == module_type::mem_tile) ? "memory" 
                           : ((type == module_type::shim) ? "interface" : "AIE");
      tileName.append(" tile (" + std::to_string(col) + "," + std::to_string(row) + ")");

      if (aie::isInfoVerbosity()) {
        std::stringstream infoMsg;
        infoMsg << "Configuring " << tileName << " for trace using metric set " << metricSet;
        xrt_core::message::send(severity_level::info, "XRT", infoMsg.str());
      }

      // Store location to flush at end of run
      if (type == module_type::core || (type == module_type::mem_tile) 
          || (type == module_type::shim)) {
        if (type == module_type::core)
          traceFlushLocs.push_back(loc);
        else if (type == module_type::mem_tile)
          memoryTileTraceFlushLocs.push_back(loc);
        else if (type == module_type::shim)
          interfaceTileTraceFlushLocs.push_back(loc);
      }

      // AIE config object for this tile
      auto cfgTile = std::make_unique<aie_cfg_tile>(col, row, type);
      cfgTile->type = type;
      cfgTile->trace_metric_set = metricSet;
      cfgTile->active_core = tile.active_core;
      cfgTile->active_memory = tile.active_memory;

      // Catch core execution trace
      if ((type == module_type::core) && (metricSet == "execution")) {
        // Set start/end events, use execution packets, and start trace module 
        XAie_TraceStopEvent(&aieDevInst, loc, XAIE_CORE_MOD, coreTraceEndEvent);

        // Driver requires at least one, non-zero trace event
        XAie_TraceEvent(&aieDevInst, loc, XAIE_CORE_MOD, XAIE_EVENT_TRUE_CORE, 0);
        
        XAie_Packet pkt = {0, 0};
        XAie_TraceModeConfig(&aieDevInst, loc, XAIE_CORE_MOD, XAIE_TRACE_INST_EXEC);
        XAie_TracePktConfig(&aieDevInst, loc, XAIE_CORE_MOD, pkt);

        if(startType != "layer" || startLayer ==  UINT_MAX)
          XAie_TraceStartEvent(&aieDevInst, loc, XAIE_CORE_MOD, coreTraceStartEvent);
        (db->getStaticInfo()).addAIECfgTile(deviceId, cfgTile);
        continue;
      }
      
      // Get vector of pre-defined metrics for this set
      // NOTE: these are local copies as we are adding tile/counter-specific events
      EventVector coreEvents;
      EventVector memoryEvents;
      EventVector interfaceEvents;
      if (type == module_type::core) {
        coreEvents = coreEventSets[metricSet];
        memoryEvents = memoryEventSets[metricSet];
      }
      else if (type == module_type::mem_tile) {
        memoryEvents = memoryTileEventSets[metricSet];
      }
      else if (type == module_type::shim) {
        interfaceEvents = interfaceTileEventSets[metricSet];
      }

      if (coreEvents.empty() && memoryEvents.empty() && interfaceEvents.empty()) {
        std::stringstream msg;
        msg << "Event trace is not available for " << tileName << " using metric set "
            << metricSet << " on hardware generation " << metadata->getHardwareGen() << ".";
        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        continue;
      }

      if (xrt_core::config::get_verbosity() >= static_cast<uint32_t>(severity_level::info)) {
        std::stringstream infoMsg;
        auto tileName = (type == module_type::mem_tile) ? "memory" 
            : ((type == module_type::shim) ? "interface" : "AIE");
        infoMsg << "Configuring " << tileName << " tile (" << +col << "," 
                << +row << ") for trace using metric set " << metricSet;
        xrt_core::message::send(severity_level::info, "XRT", infoMsg.str());
      }

      int numCoreTraceEvents = 0;
      int numMemoryTraceEvents = 0;
      int numInterfaceTraceEvents = 0;

      //
      // 1. Configure Core Trace Events
      //
      if (type == module_type::core) {
        xrt_core::message::send(severity_level::info, "XRT", "Configuring Core Trace Events");

        XAie_ModuleType mod = XAIE_CORE_MOD;
        uint16_t phyEvent = 0;

        // Configure combo & group events (e.g., DMA monitoring/group masks)
        auto comboEvents = configComboEvents(loc, mod, type, metricSet, cfgTile->core_trace_config);
        (void)comboEvents;
        configGroupEvents(loc, mod, type, metricSet);

        // Set end event for trace capture
        // NOTE: This needs to be done first
        if (XAie_TraceStopEvent(&aieDevInst, loc, mod, coreTraceEndEvent) != XAIE_OK)
          break;

        // Program core trace event slots
        for (uint8_t i = 0; i < coreEvents.size(); ++i) {
          if (XAie_TraceEvent(&aieDevInst, loc, mod, coreEvents[i], i) != XAIE_OK)
            break;

          ++numCoreTraceEvents;

          // Update config file
          XAie_EventLogicalToPhysicalConv(&aieDevInst, loc, mod, coreEvents[i], &phyEvent);
          cfgTile->core_trace_config.traced_events[i] = phyEvent;
        }
        
        // Update config file
        XAie_EventLogicalToPhysicalConv(&aieDevInst, loc, mod, coreTraceStartEvent, &phyEvent);
        cfgTile->core_trace_config.start_event = phyEvent;
        XAie_EventLogicalToPhysicalConv(&aieDevInst, loc, mod, coreTraceEndEvent, &phyEvent);
        cfgTile->core_trace_config.stop_event = phyEvent;

        coreEvents.clear();
        mNumTileTraceEvents[typeInt][numCoreTraceEvents]++;

        XAie_Packet pkt = {0, 0}; // core trace uses PC packets
        if (XAie_TraceModeConfig(&aieDevInst, loc, mod, XAIE_TRACE_EVENT_PC) != XAIE_OK)
          break;
        if (XAie_TracePktConfig(&aieDevInst, loc, mod, pkt) != XAIE_OK)
          break;
        if (startType != "layer" || startLayer == UINT_MAX)
          XAie_TraceStartEvent(&aieDevInst, loc, mod, coreTraceStartEvent);
      } // Core modules

      //
      // 2. Configure Memory Trace Events
      //
      // Applicable to memory module in AIE tiles and memory tiles
      if ((type == module_type::core) || (type == module_type::mem_tile)) {
        xrt_core::message::send(severity_level::info, "XRT", "Configuring Memory Trace Events");

        XAie_ModuleType mod = XAIE_MEM_MOD;
        uint8_t firstBroadcastId = 8;

        XAie_Events traceStartEvent =
          (type == module_type::core) ? coreTraceStartEvent : memoryTileTraceStartEvent;
        XAie_Events traceEndEvent =
          (type == module_type::core) ? coreTraceEndEvent : memoryTileTraceEndEvent;

        aie_cfg_base& aieConfig = cfgTile->core_trace_config;
        if (type == module_type::mem_tile)
          aieConfig = cfgTile->memory_tile_trace_config;

        // Combo event override (for DMA metric sets)
        auto comboEvents = configComboEvents(loc, mod, type, metricSet, aieConfig);
        if (comboEvents.size() == 2) {
          traceStartEvent = comboEvents.at(0);
          traceEndEvent = comboEvents.at(1);
        }
        else if (type == module_type::core) {
          // Route core start/stop into memory-module trace via broadcast (same as client/NPU3).
          if (!m_trace_start_broadcast) {
            if (XAie_EventBroadcast(&aieDevInst, loc, XAIE_CORE_MOD, 8, traceStartEvent) != XAIE_OK)
              break;
          }
          if (XAie_EventBroadcast(&aieDevInst, loc, XAIE_CORE_MOD, 9, traceEndEvent) != XAIE_OK)
            break;

          uint16_t phyBroadcast = 0;
          if (!m_trace_start_broadcast) {
            XAie_EventLogicalToPhysicalConv(&aieDevInst, loc, XAIE_CORE_MOD, traceStartEvent, &phyBroadcast);
            cfgTile->core_trace_config.internal_events_broadcast[8] = phyBroadcast;
          }
          XAie_EventLogicalToPhysicalConv(&aieDevInst, loc, XAIE_CORE_MOD, traceEndEvent, &phyBroadcast);
          cfgTile->core_trace_config.internal_events_broadcast[9] = phyBroadcast;

          if (m_trace_start_broadcast)
            traceStartEvent =
              static_cast<XAie_Events>(XAIE_EVENT_BROADCAST_0_MEM + traceStartBroadcastChId1);
          else
            traceStartEvent = XAIE_EVENT_BROADCAST_8_MEM;
          traceEndEvent = XAIE_EVENT_BROADCAST_9_MEM;
          firstBroadcastId = 10;
        }

        if (type == module_type::core) {
          if (XAie_EventBroadcastBlockMapDir(&aieDevInst, loc, XAIE_CORE_MOD, XAIE_EVENT_SWITCH_A, 0xFF00,
                                             XAIE_EVENT_BROADCAST_WEST | XAIE_EVENT_BROADCAST_NORTH |
                                                 XAIE_EVENT_BROADCAST_SOUTH) != XAIE_OK)
            break;
          if (XAie_EventBroadcastBlockMapDir(&aieDevInst, loc, XAIE_MEM_MOD, XAIE_EVENT_SWITCH_A, 0xFF00,
                                             XAIE_EVENT_BROADCAST_EAST | XAIE_EVENT_BROADCAST_NORTH |
                                                 XAIE_EVENT_BROADCAST_SOUTH) != XAIE_OK)
            break;

          for (uint8_t bi = 8; bi < 16; bi++) {
            if (XAie_EventBroadcastUnblockDir(&aieDevInst, loc, XAIE_CORE_MOD, XAIE_EVENT_SWITCH_A, bi,
                                                XAIE_EVENT_BROADCAST_EAST) != XAIE_OK)
              break;
          }
        }

        // Configure stream switch ports (core SS DMA monitors feeding MEM-side trace)
        configStreamSwitchPorts(tile, loc, type, metricSet, 0, 0, memoryEvents, aieConfig);

        memoryModTraceStartEvent = traceStartEvent;
        if (XAie_TraceStopEvent(&aieDevInst, loc, mod, traceEndEvent) != XAIE_OK)
          break;

        {
          uint16_t phyEvent1 = 0;
          uint16_t phyEvent2 = 0;
          XAie_EventLogicalToPhysicalConv(&aieDevInst, loc, mod, traceStartEvent, &phyEvent1);
          XAie_EventLogicalToPhysicalConv(&aieDevInst, loc, mod, traceEndEvent, &phyEvent2);
          if (type == module_type::core) {
            cfgTile->memory_trace_config.start_event = phyEvent1;
            cfgTile->memory_trace_config.stop_event = phyEvent2;
          } else {
            cfgTile->memory_tile_trace_config.start_event = phyEvent1;
            cfgTile->memory_tile_trace_config.stop_event = phyEvent2;
          }
        }

        auto iter0 = configChannel0.find(tile);
        auto iter1 = configChannel1.find(tile);
        uint8_t channel0 = (iter0 == configChannel0.end()) ? 0 : iter0->second;
        uint8_t channel1 = (iter1 == configChannel1.end()) ? 1 : iter1->second;

        if (type == module_type::mem_tile) {
          configEventSelections(tile, loc, type, metricSet, channel0, channel1, cfgTile->memory_tile_trace_config);
        } else {
          if (!memoryEvents.empty()) {
            auto channelNum = aie::getChannelNumberFromEvent(memoryEvents.at(0));
            if (channelNum >= 0) {
              if (aie::isInputSet(type, metricSet)) {
                cfgTile->core_trace_config.mm2s_channels[0] = channelNum;
                if (static_cast<size_t>(channelNum) < tile.mm2s_names.size())
                  cfgTile->core_trace_config.mm2s_names[0] = tile.mm2s_names.at(channelNum);
              } else {
                cfgTile->core_trace_config.s2mm_channels[0] = channelNum;
                if (static_cast<size_t>(channelNum) < tile.s2mm_names.size())
                  cfgTile->core_trace_config.s2mm_names[0] = tile.s2mm_names.at(channelNum);
              }
            }
          }
        }

        uint8_t bcId = firstBroadcastId;
        int bcIndex = (firstBroadcastId == 10) ? 2 : 0;
        static const XAie_Events kMemBcEv[] = {
          XAIE_EVENT_BROADCAST_8_MEM,  XAIE_EVENT_BROADCAST_9_MEM,  XAIE_EVENT_BROADCAST_10_MEM,
          XAIE_EVENT_BROADCAST_11_MEM, XAIE_EVENT_BROADCAST_12_MEM, XAIE_EVENT_BROADCAST_13_MEM,
          XAIE_EVENT_BROADCAST_14_MEM, XAIE_EVENT_BROADCAST_15_MEM};

        for (uint8_t i = 0; i < memoryEvents.size(); i++) {
          const bool isCoreEvent = xdp::aie::isCoreModuleEvent(memoryEvents[i]);

          if (isCoreEvent) {
            if (XAie_EventBroadcast(&aieDevInst, loc, XAIE_CORE_MOD, bcId, memoryEvents[i]) != XAIE_OK)
              break;
            if (bcIndex >= static_cast<int>(sizeof(kMemBcEv) / sizeof(kMemBcEv[0])))
              break;
            if (XAie_TraceEvent(&aieDevInst, loc, XAIE_MEM_MOD, kMemBcEv[bcIndex++], i) != XAIE_OK)
              break;
          } else {
            if (XAie_TraceEvent(&aieDevInst, loc, XAIE_MEM_MOD, memoryEvents[i], i) != XAIE_OK)
              break;
          }

          ++numMemoryTraceEvents;

          configEdgeEvents(tile, type, metricSet, memoryEvents[i], channel0);

          uint16_t phyEvent = 0;
          const XAie_ModuleType phyModConv = isCoreEvent ? XAIE_CORE_MOD : XAIE_MEM_MOD;
          XAie_EventLogicalToPhysicalConv(&aieDevInst, loc, phyModConv, memoryEvents[i], &phyEvent);

          if (isCoreEvent) {
            cfgTile->core_trace_config.internal_events_broadcast[bcId] = phyEvent;
            cfgTile->memory_trace_config.traced_events[i] = bcIdToEvent(bcId);
            ++bcId;
          } else if (type == module_type::mem_tile) {
            cfgTile->memory_tile_trace_config.traced_events[i] = phyEvent;
          } else {
            cfgTile->memory_trace_config.traced_events[i] = phyEvent;
          }
        }

        memoryEvents.clear();
        mNumTileTraceEvents[typeInt][numMemoryTraceEvents]++;

        uint8_t packetType = (type == module_type::mem_tile) ? 3 : 1;
        XAie_Packet pkt = {0, packetType};

        if (XAie_TracePktConfig(&aieDevInst, loc, mod, pkt) != XAIE_OK)
          break;
        if ((startType != "layer") || (startLayer == UINT_MAX)) {
          if (XAie_TraceStartEvent(&aieDevInst, loc, mod, traceStartEvent) != XAIE_OK)
            break;
        }

        if (type == module_type::mem_tile)
          cfgTile->memory_tile_trace_config.packet_type = packetType;
        else
          cfgTile->memory_trace_config.packet_type = packetType;
      } // Memory modules/tiles

      //
      // 3. Configure Interface Tile Trace Events
      //
      if (type == module_type::shim) {
        xrt_core::message::send(severity_level::info, "XRT", "Configuring Interface Tile Trace Events");
        XAie_ModuleType mod = XAIE_PL_MOD;

        auto iter0 = configChannel0.find(tile);
        auto iter1 = configChannel1.find(tile);
        uint8_t channel0 = (iter0 == configChannel0.end()) ? 0 : iter0->second;
        uint8_t channel1 = (iter1 == configChannel1.end()) ? 1 : iter1->second;
        std::vector<uint8_t> channels = {channel0, channel1, 2, 3};

        modifyEvents(type, subtype, metricSet, channel0, interfaceEvents);
        configEventSelections(tile, loc, type, metricSet, channel0, channel1, cfgTile->interface_tile_trace_config);
        configStreamSwitchPorts(tileMetric.first, loc, type, metricSet, channel0, channel1,
                                interfaceEvents, cfgTile->interface_tile_trace_config);
        
        // Configure interface tile trace events
        for (size_t i = 0; i < interfaceEvents.size(); ++i) {
          auto event = interfaceEvents.at(i);
          if (XAie_TraceEvent(&aieDevInst, loc, mod, event, static_cast<uint8_t>(i)) != XAIE_OK)
            break;

          ++numInterfaceTraceEvents;

          uint16_t phyEvent = 0;
          XAie_EventLogicalToPhysicalConv(&aieDevInst, loc, XAIE_PL_MOD, event, &phyEvent);
          cfgTile->interface_tile_trace_config.traced_events[i] = phyEvent;
        }

        // Update config file
        {
          // Add interface trace control events
          // Start
          uint16_t phyEvent = 0;
          XAie_EventLogicalToPhysicalConv(&aieDevInst, loc, XAIE_PL_MOD, interfaceTileTraceStartEvent, &phyEvent);
          cfgTile->interface_tile_trace_config.start_event = phyEvent;
          // Stop
          XAie_EventLogicalToPhysicalConv(&aieDevInst, loc, XAIE_PL_MOD, interfaceTileTraceEndEvent, &phyEvent);
          cfgTile->interface_tile_trace_config.stop_event = phyEvent;
        }

        mNumTileTraceEvents[typeInt][numInterfaceTraceEvents]++;

        uint8_t packetType = 4;
        XAie_Packet pkt = {0, packetType};
        if (XAie_TracePktConfig(&aieDevInst, loc, mod, pkt) != XAIE_OK)
          break;
        if (startType != "layer" || startLayer == UINT_MAX) {
          if (XAie_TraceStartEvent(&aieDevInst, loc, mod, interfaceTileTraceStartEvent) != XAIE_OK)
            break;
        }
        if (XAie_TraceStopEvent(&aieDevInst, loc, mod, interfaceTileTraceEndEvent) != XAIE_OK)
          break;

        cfgTile->interface_tile_trace_config.packet_type = packetType;

        if (!interfaceEvents.empty()) {
          auto channelNum = aie::getChannelNumberFromEvent(interfaceEvents.at(0));
          if (channelNum >= 0) {
            if (aie::isInputSet(type, metricSet))
              cfgTile->interface_tile_trace_config.mm2s_channels[channelNum] = channelNum;
            else
              cfgTile->interface_tile_trace_config.s2mm_channels[channelNum] = channelNum;
          }
        }
      } // Interface tiles

      if (xrt_core::config::get_verbosity() >= static_cast<uint32_t>(severity_level::debug)) {
        std::stringstream msg;
        msg << "Reserved ";
        if (type == module_type::core)
          msg << numCoreTraceEvents << " core and " << numMemoryTraceEvents << " memory";
        else if (type == module_type::mem_tile)
          msg << numMemoryTraceEvents << " memory tile";
        else if (type == module_type::shim)
          msg << numInterfaceTraceEvents << " interface tile";
        msg << " trace events for tile (" << +col << "," << +row 
            << "). Adding tile to static database.";
        xrt_core::message::send(severity_level::debug, "XRT", msg.str());
      }

      // Add config info to static database
      // NOTE: Do not access cfgTile after this
      (db->getStaticInfo()).addAIECfgTile(deviceId, cfgTile);
      xrt_core::message::send(severity_level::info, "XRT", "Debugging XDP: after (db->getStaticInfo()).addAIECfgTile");  
    }  // For tiles

    // Report and store trace events per tile
    for (int m = 0; m < static_cast<int>(module_type::num_types); ++m) {
      aie::trace::printTraceEventStats(m, mNumTileTraceEvents[m]);
      for (int n = 0; n <= NUM_TRACE_EVENTS; ++n)
        (db->getStaticInfo()).addAIECoreEventResources(deviceId, n, mNumTileTraceEvents[m][n]);
    }

    if (m_trace_start_broadcast) {
      xrt_core::message::send(severity_level::info, "XRT", "before build2ChannelBroadcastNetwork");  
      build2ChannelBroadcastNetwork(handle, traceStartBroadcastChId1, traceStartBroadcastChId2, interfaceTileTraceStartEvent);
      xrt_core::message::send(severity_level::info, "XRT", "before XAie_EventGenerate");
      XAie_EventGenerate(&aieDevInst, XAie_TileLoc(startCol, 0), XAIE_PL_MOD,  interfaceTileTraceStartEvent);
      reset2ChannelBroadcastNetwork(handle, traceStartBroadcastChId1, traceStartBroadcastChId2);
    }

    auto hwContextSubmit = metadata->getHwContext();
    if (!tranxHandler->submitTransaction(&aieDevInst, hwContextSubmit)) {
      xrt_core::message::send(severity_level::error, "XRT",
        "Aie trace control-code transaction submission failed.");
      return false;
    }
    xrt_core::message::send(severity_level::info, "XRT", "Successfully scheduled AIE Trace.");

    if (!tranxHandler->initializeTransaction(&aieDevInst, "AieTraceFlush")) {
      xrt_core::message::send(severity_level::error, "XRT",
        "AIE trace flush transaction initialization failed.");
      return false;
    }

    // Flush trace by forcing end event
    // NOTE: this informs tiles to output remaining packets (even if partial)
    for (const auto& loc : traceFlushLocs) 
      XAie_EventGenerate(&aieDevInst, loc, XAIE_CORE_MOD, coreTraceEndEvent);
    for (const auto& loc : memoryTileTraceFlushLocs)
      XAie_EventGenerate(&aieDevInst, loc, XAIE_MEM_MOD, memoryTileTraceEndEvent);
    for (const auto& loc : interfaceTileTraceFlushLocs)
      XAie_EventGenerate(&aieDevInst, loc, XAIE_PL_MOD, interfaceTileTraceEndEvent);

    tranxHandler->completeASM(&aieDevInst);
    if (!tranxHandler->generateELF()) {
      xrt_core::message::send(severity_level::error, "XRT",
        "AIE trace flush ELF generation failed.");
      return false;
    }

    xrt_core::message::send(severity_level::info, "XRT", "Successfully generated ELF for AIE Trace Flush.");

    return true;
  }  // end setMetricsSettings

  /****************************************************************************
   * Flush trace modules by forcing end events
   *
   * Trace modules buffer partial packets. At end of run, this needs to be 
   * flushed using a custom end event. This applies to trace windowing and 
   * passive tiles like memory and interface.
   *
   ***************************************************************************/
  void AieTrace_VE2Impl::flushTraceModules()
  {
    //if (db->infoAvailable(xdp::info::ml_timeline)) {
    //  db->broadcast(VPDatabase::MessageType::READ_RECORD_TIMESTAMPS, nullptr);
    //  xrt_core::message::send(severity_level::debug, "XRT", "Done reading recorded timestamps.");
    //}

    if (traceFlushLocs.empty() && memoryTileTraceFlushLocs.empty()
        && interfaceTileTraceFlushLocs.empty())
      return;

    if (aie::isDebugVerbosity()) {
      std::stringstream msg;
      msg << "Flushing AIE trace by forcing end event for " << traceFlushLocs.size()
          << " AIE tiles, " << memoryTileTraceFlushLocs.size() << " memory tiles, and " 
          << interfaceTileTraceFlushLocs.size() << " interface tiles.";
      xrt_core::message::send(severity_level::debug, "XRT", msg.str());
    }

    traceFlushLocs.clear();
    memoryTileTraceFlushLocs.clear();
    interfaceTileTraceFlushLocs.clear();

    xrt_core::message::send(severity_level::info, "XRT", "Before AIE trace flush.");
    auto hwContext = metadata->getHwContext();
    if (!tranxHandler->submitELF(hwContext)) {
      xrt_core::message::send(severity_level::warning, "XRT",
        "AIE trace flush control-code submission failed.");
      return;
    }
    xrt_core::message::send(severity_level::info, "XRT", "Successfully scheduled AIE trace flush.");
  }

  /***************************************************************************
   * Build broadcast network using specified channels 
   ***************************************************************************/
  void AieTrace_VE2Impl::build2ChannelBroadcastNetwork(void *hwCtxImpl, uint8_t broadcastId1,
                                                       uint8_t broadcastId2, XAie_Events event)
  {
    boost::property_tree::ptree aiePartitionPt = xdp::aie::getAIEPartitionInfo(hwCtxImpl);
    if (aiePartitionPt.empty()) {
      xrt_core::message::send(severity_level::warning, "XRT",
                              "AIE trace: no partition info for trace-start broadcast; skipping broadcast network.");
      return;
    }
    // Currently, assuming only one Hw Context is alive at a time
    // uint8_t startCol = static_cast<uint8_t>(aiePartitionPt.front().second.get<uint64_t>("start_col"));
    uint8_t startCol = 0;
    // uint8_t numCols  = static_cast<uint8_t>(aiePartitionPt.front().second.get<uint64_t>("num_cols"));
    uint8_t numCols = 36;
    const uint8_t startColShift = metadata->getPartitionOverlayStartCols().front();

    std::vector<uint8_t> maxRowAtCol(startCol + numCols, 0);
    for (auto& tileMetric : metadata->getConfigMetrics()) {
      auto tile       = tileMetric.first;
      auto col        = tile.col;
      auto row        = tile.row;
      maxRowAtCol[startCol + col] = std::max(maxRowAtCol[col], (uint8_t)row);
    }

    XAie_Events bcastEvent2_PL = static_cast<XAie_Events>(XAIE_EVENT_BROADCAST_A_0_PL + broadcastId2);
    XAie_EventBroadcast(&aieDevInst, XAie_TileLoc(startCol, 0), XAIE_PL_MOD, broadcastId2, event);

    for (uint8_t col = startCol; col < (startCol + numCols); col++) {
      for (uint8_t row = 0; row <= maxRowAtCol[col]; row++) {
        module_type tileType = aie::getModuleType(row, metadata->getRowOffset());
        auto loc = XAie_TileLoc(col, row);

        // shim tile
        if (tileType == module_type::shim) {
          // first channel is only used to send north
          if (col == startCol) {
            XAie_EventBroadcast(&aieDevInst, loc, XAIE_PL_MOD, broadcastId1, event);
          } else {
            XAie_EventBroadcast(&aieDevInst, loc, XAIE_PL_MOD, broadcastId1, bcastEvent2_PL);
          }
          if (maxRowAtCol[col] != row) {
            XAie_EventBroadcastBlockDir(&aieDevInst, loc, XAIE_PL_MOD, XAIE_EVENT_SWITCH_A, broadcastId1,
                                        XAIE_EVENT_BROADCAST_SOUTH | XAIE_EVENT_BROADCAST_WEST | XAIE_EVENT_BROADCAST_EAST);
          } else {
            XAie_EventBroadcastBlockDir(
                &aieDevInst, loc, XAIE_PL_MOD, XAIE_EVENT_SWITCH_A, broadcastId1,
                XAIE_EVENT_BROADCAST_SOUTH | XAIE_EVENT_BROADCAST_WEST | XAIE_EVENT_BROADCAST_EAST | XAIE_EVENT_BROADCAST_NORTH);
          }

          // second channel is only used to send east
          if (col != startCol + numCols - 1) {
            XAie_EventBroadcastBlockDir(&aieDevInst, loc, XAIE_PL_MOD, XAIE_EVENT_SWITCH_A, broadcastId2,
                                        XAIE_EVENT_BROADCAST_SOUTH | XAIE_EVENT_BROADCAST_WEST | XAIE_EVENT_BROADCAST_NORTH);
          } else {
            XAie_EventBroadcastBlockDir(&aieDevInst, loc, XAIE_PL_MOD, XAIE_EVENT_SWITCH_A, broadcastId2,
                                        XAIE_EVENT_BROADCAST_SOUTH | XAIE_EVENT_BROADCAST_WEST | XAIE_EVENT_BROADCAST_NORTH);
          }
        } 
        
        // mem tile
        else if (tileType == module_type::mem_tile) {
          if (maxRowAtCol[col] != row) {
            XAie_EventBroadcastBlockDir(&aieDevInst, loc, XAIE_MEM_MOD, XAIE_EVENT_SWITCH_A, broadcastId1,
                                        XAIE_EVENT_BROADCAST_SOUTH | XAIE_EVENT_BROADCAST_WEST | XAIE_EVENT_BROADCAST_EAST);
          } else {
            XAie_EventBroadcastBlockDir(
                &aieDevInst, loc, XAIE_MEM_MOD, XAIE_EVENT_SWITCH_A, broadcastId1,
                XAIE_EVENT_BROADCAST_SOUTH | XAIE_EVENT_BROADCAST_WEST | XAIE_EVENT_BROADCAST_EAST | XAIE_EVENT_BROADCAST_NORTH);
          }
        } 
        
        // core tile
        else { 
          if (maxRowAtCol[col] != row) {
            XAie_EventBroadcastBlockDir(&aieDevInst, loc, XAIE_CORE_MOD, XAIE_EVENT_SWITCH_A, broadcastId1,
                                        XAIE_EVENT_BROADCAST_SOUTH | XAIE_EVENT_BROADCAST_WEST | XAIE_EVENT_BROADCAST_EAST);
          } else {
            XAie_EventBroadcastBlockDir(
                &aieDevInst, loc, XAIE_CORE_MOD, XAIE_EVENT_SWITCH_A, broadcastId1,
                XAIE_EVENT_BROADCAST_SOUTH | XAIE_EVENT_BROADCAST_WEST | XAIE_EVENT_BROADCAST_EAST | XAIE_EVENT_BROADCAST_NORTH);
          }
        }
      }
    }
  }

  /***************************************************************************
   * Reset using broadcast network on specified channels 
   ***************************************************************************/
  void AieTrace_VE2Impl::reset2ChannelBroadcastNetwork(void *hwCtxImpl, uint8_t broadcastId1,
                                                       uint8_t broadcastId2)
  {
    boost::property_tree::ptree aiePartitionPt = xdp::aie::getAIEPartitionInfo(hwCtxImpl);
    if (aiePartitionPt.empty()) {
      xrt_core::message::send(severity_level::warning, "XRT",
        "AIE trace: no partition info for trace-start broadcast reset; skipping.");
      return;
    }
    // Currently, assuming only one Hw Context is alive at a time
    //uint8_t startCol = static_cast<uint8_t>(aiePartitionPt.back().second.get<uint64_t>("start_col"));
    uint8_t startCol = 0;
    //uint8_t numCols  = static_cast<uint8_t>(aiePartitionPt.back().second.get<uint64_t>("num_cols"));
    uint8_t numCols = 36;
    const uint8_t startColShift = metadata->getPartitionOverlayStartCols().front();

    std::vector<uint8_t> maxRowAtCol(startCol + numCols, 0);
    for (auto& tileMetric : metadata->getConfigMetrics()) {
      auto tile = tileMetric.first;
      auto col        = tile.col;
      auto row        = tile.row;
      maxRowAtCol[startCol + col] = std::max(maxRowAtCol[col], (uint8_t)row);
    }

    XAie_EventBroadcastReset(&aieDevInst, XAie_TileLoc(startCol, 0), XAIE_PL_MOD, broadcastId2);

    for (uint8_t col = startCol; col < (startCol + numCols); col++) {
      for (uint8_t row = 0; row <= maxRowAtCol[col]; row++) {
        module_type tileType = aie::getModuleType(row, metadata->getRowOffset());
        auto loc = XAie_TileLoc(col, row);

        // shim tile
        if (tileType == module_type::shim) {
          XAie_EventBroadcastReset(&aieDevInst, loc, XAIE_PL_MOD, broadcastId1);
          XAie_EventBroadcastUnblockDir(&aieDevInst, loc, XAIE_PL_MOD, XAIE_EVENT_SWITCH_A, broadcastId1,
                                      XAIE_EVENT_BROADCAST_ALL);
          XAie_EventBroadcastUnblockDir(&aieDevInst, loc, XAIE_PL_MOD, XAIE_EVENT_SWITCH_A, broadcastId2,
                                      XAIE_EVENT_BROADCAST_ALL);
          XAie_EventBroadcastUnblockDir(&aieDevInst, loc, XAIE_PL_MOD, XAIE_EVENT_SWITCH_B, broadcastId2,
                                      XAIE_EVENT_BROADCAST_ALL);
        } 
        
        // mem tile
        else if (tileType == module_type::mem_tile) {
          XAie_EventBroadcastUnblockDir(&aieDevInst, loc, XAIE_MEM_MOD, XAIE_EVENT_SWITCH_A, broadcastId1,
                                      XAIE_EVENT_BROADCAST_ALL);
        } 
        
        // core tile
        else {
          XAie_EventBroadcastUnblockDir(&aieDevInst, loc, XAIE_CORE_MOD, XAIE_EVENT_SWITCH_A, broadcastId1,
                                      XAIE_EVENT_BROADCAST_ALL);
        }
      }
    }
  }

  /****************************************************************************
   * Modify events in metric set based on type and channel 
   ***************************************************************************/
  void AieTrace_VE2Impl::modifyEvents(module_type type, io_type subtype,
                                      const std::string metricSet, uint8_t channel,
                                      std::vector<XAie_Events>& events)
  {
    // Only needed for GMIO DMA channel 1
    if ((type != module_type::shim) || (subtype == io_type::PLIO) || (channel == 0))
      return;

    // Check type to minimize replacements
    if (aie::isInputSet(type, metricSet)) {
      // Input or MM2S
      std::replace(events.begin(), events.end(),
                   XAIE_EVENT_NOC0_DMA_MM2S_0_START_TASK_PL, XAIE_EVENT_NOC0_DMA_MM2S_1_START_TASK_PL);
      std::replace(events.begin(), events.end(),
                   XAIE_EVENT_NOC0_DMA_MM2S_0_FINISHED_BD_PL, XAIE_EVENT_NOC0_DMA_MM2S_1_FINISHED_BD_PL);
      std::replace(events.begin(), events.end(),
                   XAIE_EVENT_NOC0_DMA_MM2S_0_FINISHED_TASK_PL, XAIE_EVENT_NOC0_DMA_MM2S_1_FINISHED_TASK_PL);
      std::replace(events.begin(), events.end(),
                   XAIE_EVENT_NOC0_DMA_MM2S_0_STALLED_LOCK_PL, XAIE_EVENT_NOC0_DMA_MM2S_1_STALLED_LOCK_PL);
      std::replace(events.begin(), events.end(),
                   XAIE_EVENT_NOC0_DMA_MM2S_0_STREAM_BACKPRESSURE_PL, XAIE_EVENT_NOC0_DMA_MM2S_1_STREAM_BACKPRESSURE_PL);
      std::replace(events.begin(), events.end(),
                   XAIE_EVENT_NOC0_DMA_MM2S_0_MEMORY_STARVATION_PL, XAIE_EVENT_NOC0_DMA_MM2S_1_MEMORY_STARVATION_PL);
    } else {
      // Output or S2MM
      std::replace(events.begin(), events.end(),
                   XAIE_EVENT_NOC0_DMA_S2MM_0_START_TASK_PL, XAIE_EVENT_NOC0_DMA_S2MM_1_START_TASK_PL);
      std::replace(events.begin(), events.end(),
                   XAIE_EVENT_NOC0_DMA_S2MM_0_FINISHED_BD_PL, XAIE_EVENT_NOC0_DMA_S2MM_1_FINISHED_BD_PL);
      std::replace(events.begin(), events.end(),
                   XAIE_EVENT_NOC0_DMA_S2MM_0_FINISHED_TASK_PL, XAIE_EVENT_NOC0_DMA_S2MM_1_FINISHED_TASK_PL);
      std::replace(events.begin(), events.end(),
                   XAIE_EVENT_NOC0_DMA_S2MM_0_STALLED_LOCK_PL, XAIE_EVENT_NOC0_DMA_S2MM_1_STALLED_LOCK_PL);
      std::replace(events.begin(), events.end(),
                   XAIE_EVENT_NOC0_DMA_S2MM_0_STREAM_STARVATION_PL, XAIE_EVENT_NOC0_DMA_S2MM_1_STREAM_STARVATION_PL);
      std::replace(events.begin(), events.end(),
                   XAIE_EVENT_NOC0_DMA_S2MM_0_MEMORY_BACKPRESSURE_PL, XAIE_EVENT_NOC0_DMA_S2MM_1_MEMORY_BACKPRESSURE_PL);
    }
  }

  uint32_t AieTrace_VE2Impl::bcIdToEvent(int bcId)
  {
    return bcId + CORE_BROADCAST_EVENT_BASE;
  }

  /****************************************************************************
   * Configure stream switch event ports for monitoring purposes 
   ***************************************************************************/
  void
  AieTrace_VE2Impl::configStreamSwitchPorts(const tile_type& tile, const XAie_LocType loc,
                                            const module_type type, const std::string metricSet,
                                            const uint8_t channel0, const uint8_t channel1, 
                                            std::vector<XAie_Events>& events, aie_cfg_base& config)
  {
    std::set<uint8_t> portSet;

    // Traverse all counters and request monitor ports as needed
    for (int i=0; i < events.size(); ++i) {
      // Ensure applicable event
      auto event = events.at(i);
      if (!xdp::aie::isStreamSwitchPortEvent(event))
        continue;

      auto portnum = xdp::aie::getPortNumberFromEvent(event);
      uint8_t channelNum = portnum % 2;
      uint8_t channel = (channelNum == 0) ? channel0 : channel1;

      // New port needed: reserve, configure, and store
      if (portSet.find(portnum) == portSet.end()) {
        portSet.insert(portnum);

        if (type == module_type::core) {
          // AIE Tiles - Monitor DMA channels
          bool isMaster = ((portnum >= 2) || (metricSet.find("s2mm") != std::string::npos));
          auto slaveOrMaster = isMaster ? XAIE_STRMSW_MASTER : XAIE_STRMSW_SLAVE;
          std::string typeName = isMaster ? "S2MM" : "MM2S";
          std::string msg = "Configuring core module stream switch to monitor DMA " 
                          + typeName + " channel " + std::to_string(channelNum);
          xrt_core::message::send(severity_level::debug, "XRT", msg);
          XAie_EventSelectStrmPort(&aieDevInst, loc, portnum, slaveOrMaster, DMA, channelNum);

          // Record for runtime config file
          // NOTE: channel info informs back-end there will be events on that channel
          config.port_trace_ids[portnum] = channelNum;
          config.port_trace_is_master[portnum] = isMaster;
          config.port_trace_names[portnum] = tile.port_names.at(portnum);

          if (isMaster) {
            config.s2mm_channels[channelNum] = channelNum;
            if (channelNum < tile.s2mm_names.size())
              config.s2mm_names[channelNum] = tile.s2mm_names.at(channelNum);
          }
          else {
            config.mm2s_channels[channelNum] = channelNum;
            if (channelNum < tile.mm2s_names.size())
              config.mm2s_names[channelNum] = tile.mm2s_names.at(channelNum);
          }
        }
        // Interface tiles (e.g., PLIO, GMIO)
        else if (type == module_type::shim) {
          // NOTE: skip configuration of extra ports for tile if stream_ids are not available.
          if (portnum >= tile.stream_ids.size())
            continue;

          auto slaveOrMaster = (tile.is_master_vec.at(portnum) == 0)   ? XAIE_STRMSW_SLAVE : XAIE_STRMSW_MASTER;
          std::string typeName = (tile.is_master_vec.at(portnum) == 0) ? "slave" : "master";
          uint8_t streamPortId = static_cast<uint8_t>(tile.stream_ids.at(portnum));

          std::string msg = "Configuring interface tile stream switch to monitor " 
                          + typeName + " stream port " + std::to_string(streamPortId);
          xrt_core::message::send(severity_level::debug, "XRT", msg);
          XAie_EventSelectStrmPort(&aieDevInst, loc, portnum, slaveOrMaster, SOUTH, streamPortId);

          // Record for runtime config file
          config.port_trace_ids[portnum] = (tile.subtype == io_type::PLIO) ? portnum : channel;
          config.port_trace_is_master[portnum] = (tile.is_master_vec.at(portnum) != 0);
          if (tile.subtype == io_type::PLIO) {
            if (streamPortId < tile.port_names.size())
              config.port_trace_names[portnum] = tile.port_names.at(streamPortId);
          } else {
            if (channel < tile.port_names.size())
              config.port_trace_names[portnum] = tile.port_names.at(channel);
          }

          if (tile.is_master_vec.at(portnum) == 0) {
            config.mm2s_channels[channelNum] = channel;
            if (channelNum < tile.mm2s_names.size())
              config.mm2s_names[channelNum] = tile.mm2s_names.at(channelNum);
          }
          else {
            config.s2mm_channels[channelNum] = channel;
            if (channelNum < tile.s2mm_names.size())
              config.s2mm_names[channelNum] = tile.s2mm_names.at(channelNum);
          }
        }
        else {
          // Memory tiles
          auto slaveOrMaster = aie::isInputSet(type, metricSet) ? XAIE_STRMSW_MASTER : XAIE_STRMSW_SLAVE;
          std::string typeName = (slaveOrMaster == XAIE_STRMSW_MASTER) ? "master" : "slave";
          std::string msg = "Configuring memory tile stream switch to monitor " 
                          + typeName + " stream port " + std::to_string(channel);
          xrt_core::message::send(severity_level::debug, "XRT", msg);
          XAie_EventSelectStrmPort(&aieDevInst, loc, portnum, slaveOrMaster, DMA, channel);

          // Record for runtime config file
          config.port_trace_ids[portnum] = channel;
          config.port_trace_is_master[portnum] = (slaveOrMaster == XAIE_STRMSW_MASTER);
          if (portnum < tile.port_names.size())
            config.port_trace_names[portnum] = tile.port_names.at(portnum);
        }
      }
    }

    if ((type == module_type::shim) && (tile.subtype == io_type::PLIO) &&
        (portSet.size() < tile.stream_ids.size())) {
      std::string msg = "Interface tile " + std::to_string(tile.col) + " has more "
                      + "PLIO than can be monitored by metric set " + metricSet + ". Please "
                      + "run again with different trace settings or choose a different set.";
      xrt_core::message::send(severity_level::warning, "XRT", msg);
    }

    portSet.clear();
  }

  /****************************************************************************
  * Configure combo events (AIE tiles only) 
  ***************************************************************************/
  std::vector<XAie_Events>
  AieTrace_VE2Impl::configComboEvents(const XAie_LocType loc, const XAie_ModuleType mod,
                                      const module_type type, const std::string metricSet,
                                      aie_cfg_base& config)
  {
    // Only needed for core/memory modules and metric sets that include DMA events
    if (!xdp::aie::isDmaSet(metricSet) || ((type != module_type::core) && (type != module_type::dma)))
      return {};

    std::vector<XAie_Events> comboEvents;

    if (mod == XAIE_CORE_MOD) {
      comboEvents.push_back(XAIE_EVENT_COMBO_EVENT_2_CORE);
    
      // Combo2 = Port_Idle_0 OR Port_Idle_1 OR Port_Idle_2 OR Port_Idle_3
      std::vector<XAie_Events> events = {XAIE_EVENT_PORT_IDLE_0_CORE,
          XAIE_EVENT_PORT_IDLE_1_CORE, XAIE_EVENT_PORT_IDLE_2_CORE,
          XAIE_EVENT_PORT_IDLE_3_CORE};
      std::vector<XAie_EventComboOps> opts = {XAIE_EVENT_COMBO_E1_OR_E2, 
          XAIE_EVENT_COMBO_E1_OR_E2, XAIE_EVENT_COMBO_E1_OR_E2};

      // Capture in config class to report later
      for (int i=0; i < NUM_COMBO_EVENT_CONTROL; ++i)
        config.combo_event_control[i] = 2;
      for (int i=0; i < events.size(); ++i) {
        uint16_t phyEvent = 0;
        XAie_EventLogicalToPhysicalConv(&aieDevInst, loc, mod, events.at(i), &phyEvent);
        config.combo_event_input[i] = phyEvent;
      }

      // Set events and trigger on OR of events
      XAie_EventComboConfig(&aieDevInst, loc, mod, XAIE_EVENT_COMBO0, opts[0], events[0], events[1]);
      XAie_EventComboConfig(&aieDevInst, loc, mod, XAIE_EVENT_COMBO1, opts[1], events[2], events[3]);
      XAie_EventComboConfig(&aieDevInst, loc, mod, XAIE_EVENT_COMBO2, opts[2], XAIE_EVENT_COMBO_EVENT_0_PL, XAIE_EVENT_COMBO_EVENT_1_PL);
      return comboEvents;
    }

    // Combo events do not auto-broadcast from core to memory module,
    // so let's avoid the complexity and find a different method. 
  #if 0
    // Below is for memory modules

    // Memory_Combo0 = (Active OR Group_Stream_Switch)
    auto comboEvent0 = xaieTile.mem().comboEvent();
    comboEvents.push_back(XAIE_EVENT_COMBO_EVENT_0_MEM);

    std::vector<XAie_Events> events0;
    events0.push_back(XAIE_EVENT_ACTIVE_CORE);
    events0.push_back(XAIE_EVENT_GROUP_STREAM_SWITCH_CORE);
    std::vector<XAie_EventComboOps> opts0;
    opts0.push_back(XAIE_EVENT_COMBO_E1_OR_E2);
    
    comboEvent0->setEvents(events0, opts0);

    // Memory_Combo1 = (Group_Core_Program_Flow AND Core_Combo2)
    auto comboEvent1 = xaieTile.mem().comboEvent();
    comboEvents.push_back(XAIE_EVENT_COMBO_EVENT_1_MEM);

    std::vector<XAie_Events> events1;
    events1.push_back(XAIE_EVENT_GROUP_CORE_PROGRAM_FLOW_CORE);
    events1.push_back(XAIE_EVENT_COMBO_EVENT_2_CORE);
    std::vector<XAie_EventComboOps> opts1;
    opts1.push_back(XAIE_EVENT_COMBO_E1_AND_E2);
    
    comboEvent1->setEvents(events1, opts1);
  #else
    // Since we're tracing DMA events, start trace right away.
    // Specify user event 0 as trace end so we can flush after run.
    comboEvents.push_back(XAIE_EVENT_TRUE_MEM);
    comboEvents.push_back(XAIE_EVENT_USER_EVENT_0_MEM);
  #endif
    return comboEvents;
  }

  /****************************************************************************
   * Configure group events (core modules only) 
   ***************************************************************************/
  void AieTrace_VE2Impl::configGroupEvents(const XAie_LocType loc, const XAie_ModuleType mod,
                                           const module_type type, const std::string metricSet)
  {
    // Only needed for core module and metric sets that include DMA events
    if (!aie::isDmaSet(metricSet) || (type != module_type::core))
      return;

    // Set masks for group events
    XAie_EventGroupControl(&aieDevInst, loc, mod, XAIE_EVENT_GROUP_CORE_PROGRAM_FLOW_CORE,
                           GROUP_CORE_FUNCTIONS_MASK);
    XAie_EventGroupControl(&aieDevInst, loc, mod, XAIE_EVENT_GROUP_CORE_STALL_CORE,
                           GROUP_CORE_STALL_MASK);
    XAie_EventGroupControl(&aieDevInst, loc, mod, XAIE_EVENT_GROUP_STREAM_SWITCH_CORE,
                           GROUP_STREAM_SWITCH_RUNNING_MASK);
  }

  /****************************************************************************
  * Configure event selection (memory tiles only) 
  ***************************************************************************/
  void AieTrace_VE2Impl::configEventSelections(const tile_type& tile, const XAie_LocType loc, const module_type type, 
                                              const std::string metricSet, const uint8_t channel0, 
                                              const uint8_t channel1, aie_cfg_base& config)
  {
    if (type != module_type::mem_tile)
      return;

    XAie_DmaDirection dmaDir = aie::isInputSet(type, metricSet) ? DMA_S2MM : DMA_MM2S;

    if (aie::isDebugVerbosity()) {
      std::string typeName = (dmaDir == DMA_S2MM) ? "S2MM" : "MM2S";
      std::string msg = "Configuring event selections for DMA " + typeName + " channels "
      + std::to_string(channel0) + " and " + std::to_string(channel1);
      xrt_core::message::send(severity_level::debug, "XRT", msg);
    }

    XAie_EventSelectDmaChannel(&aieDevInst, loc, 0, dmaDir, channel0);
    XAie_EventSelectDmaChannel(&aieDevInst, loc, 1, dmaDir, channel1);

    // Record for runtime config file
    config.port_trace_ids[0] = channel0;
    config.port_trace_ids[1] = channel1;
    if (aie::isInputSet(type, metricSet)) {
      config.port_trace_is_master[0] = true;
      config.port_trace_is_master[1] = true;
      config.s2mm_channels[0] = channel0;
      if (channel0 < tile.s2mm_names.size())
        config.s2mm_names[0] = tile.s2mm_names[channel0];
      if (channel0 != channel1) {
        config.s2mm_channels[1] = channel1;
        if (channel1 < tile.s2mm_names.size())
          config.s2mm_names[1] = tile.s2mm_names[channel1];
      }
    } 
    else {
      config.port_trace_is_master[0] = false;
      config.port_trace_is_master[1] = false;
      config.mm2s_channels[0] = channel0;
      if (channel0 < tile.mm2s_names.size())
        config.mm2s_names[0] = tile.mm2s_names[channel0];
      if (channel0 != channel1) {
        config.mm2s_channels[1] = channel1;
        if (channel1 < tile.mm2s_names.size())
          config.mm2s_names[1] = tile.mm2s_names[channel1];
      }
    }
  }

  /****************************************************************************
   * Configure edge detection events 
   ***************************************************************************/
  void AieTrace_VE2Impl::configEdgeEvents(const tile_type& tile,
                        const module_type type, const std::string metricSet, 
                        const XAie_Events event, const uint8_t channel)
  {
    if ((event != XAIE_EVENT_EDGE_DETECTION_EVENT_0_MEM_TILE)
        && (event != XAIE_EVENT_EDGE_DETECTION_EVENT_1_MEM_TILE)
        && (event != XAIE_EVENT_EDGE_DETECTION_EVENT_0_MEM)
        && (event != XAIE_EVENT_EDGE_DETECTION_EVENT_1_MEM))
      return;

    // Catch memory tiles
    if (type == module_type::mem_tile) {
      // Event is DMA_S2MM_Sel0_stream_starvation or DMA_MM2S_Sel0_stalled_lock
      uint16_t eventNum = aie::isInputSet(type, metricSet)
          ? EVENT_MEM_TILE_DMA_S2MM_SEL0_STREAM_STARVATION
          : EVENT_MEM_TILE_DMA_MM2S_SEL0_STALLED_LOCK;

      // Register Edge_Detection_event_control
      // 26    Event 1 triggered on falling edge
      // 25    Event 1 triggered on rising edge
      // 23:16 Input event for edge event 1
      // 10    Event 0 triggered on falling edge
      //  9    Event 0 triggered on rising edge
      //  7:0  Input event for edge event 0
      uint32_t edgeEventsValue = (1 << 26) + (eventNum << 16) + (1 << 9) + eventNum;

      xrt_core::message::send(severity_level::debug, "XRT",
          "Configuring memory tile edge events to detect rise and fall of event " 
          + std::to_string(eventNum));

      auto tileOffset = _XAie_GetTileAddr(&aieDevInst, tile.row, tile.col);
      XAie_Write32(&aieDevInst, tileOffset + AIE_OFFSET_EDGE_CONTROL_MEM_TILE, 
                   edgeEventsValue);
      return;
    }

    // Below is AIE tile support
    
    // Event is DMA_MM2S_stalled_lock or DMA_S2MM_stream_starvation
    uint16_t eventNum = aie::isInputSet(type, metricSet)
        ? ((channel == 0) ? EVENT_MEM_DMA_MM2S_0_STALLED_LOCK
                          : EVENT_MEM_DMA_MM2S_1_STALLED_LOCK)
        : ((channel == 0) ? EVENT_MEM_DMA_S2MM_0_STREAM_STARVATION
                          : EVENT_MEM_DMA_S2MM_1_STREAM_STARVATION);

    // Register Edge_Detection_event_control
    // 26    Event 1 triggered on falling edge
    // 25    Event 1 triggered on rising edge
    // 23:16 Input event for edge event 1
    // 10    Event 0 triggered on falling edge
    //  9    Event 0 triggered on rising edge
    //  7:0  Input event for edge event 0
    uint32_t edgeEventsValue = (1 << 26) + (eventNum << 16) + (1 << 9) + eventNum;

    xrt_core::message::send(severity_level::debug, "XRT", 
        "Configuring AIE tile edge events to detect rise and fall of event " 
        + std::to_string(eventNum));

    auto tileOffset = _XAie_GetTileAddr(&aieDevInst, tile.row, tile.col);
    XAie_Write32(&aieDevInst, tileOffset + AIE_OFFSET_EDGE_CONTROL_MEM, 
                 edgeEventsValue);
  }

  /****************************************************************************
   * Poll AIE timers (for system timeline only)
   ***************************************************************************/
  void AieTrace_VE2Impl::pollTimers(uint64_t index, void* handle)
  {
    // TODO: Poll timers (needed for system timeline only)
    (void)index;
    (void)handle;
  }
 
  /****************************************************************************
  * Set AIE device instance
  ***************************************************************************/
  void* AieTrace_VE2Impl::setAieDeviceInst(void* handle, uint64_t deviceID)
  {
    (void)handle;
    (void)deviceID;
    // XDNA has no shim-provided XAie_DevInst for offload; trace uses hw_context
    // in AIETraceOffloadManager / AIETraceOffload (see aie_trace_offload_ve2).
    return nullptr;
  }

  bool AieTrace_VE2Impl::tileHasFreeRsc(xaiefal::XAieDev* aieDevice, XAie_LocType& loc, const module_type type, const std::string& metricSet) {}
  void AieTrace_VE2Impl::freeResources() {}


}  // namespace xdp

#endif