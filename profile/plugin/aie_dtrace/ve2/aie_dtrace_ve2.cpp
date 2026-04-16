// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2022-2025 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE 

#include "xdp/profile/plugin/aie_dtrace/ve2/aie_dtrace_ve2.h"
#include "xdp/profile/plugin/aie_dtrace/ve2/aie_dtrace_ct_writer.h"
#include "xdp/profile/plugin/aie_profile/aie_profile_defs.h"
#include "xdp/profile/plugin/aie_profile/aie_profile_metadata.h"
#include "xdp/profile/plugin/aie_profile/util/aie_profile_util.h"
#include "xdp/profile/plugin/aie_profile/util/aie_profile_config.h"
#include "xdp/profile/plugin/aie_base/aie_base_util.h"
#include "xdp/profile/plugin/aie_base/aie_nop_util.h"

#include "xdp/profile/database/database.h"
#include "xdp/profile/device/utility.h"
#include "xdp/profile/database/static_info/aie_util.h"
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/database/static_info/pl_constructs.h"

#include <boost/algorithm/string.hpp>
#include <cmath>
#include <memory>
#include <cstring>
#include <map>

#include "core/common/message.h"
#include "core/common/time.h"
#include "core/common/config_reader.h"
#include "core/include/xrt/xrt_kernel.h"

#include <filesystem>
#include <iterator>
#include <sstream>
#include "core/common/shim/hwctx_handle.h"
#include "core/common/api/hw_context_int.h"
#include "xdp/profile/plugin/aie_dtrace/ve2/elf_helper.h"
#include "shim_ve2/xdna_hwctx.h"

namespace {
  static void* fetchAieDevInst(void* devHandle)
  {
    xrt::hw_context context = xrt_core::hw_context_int::create_hw_context_from_implementation(devHandle);
    auto hwctx_hdl = static_cast<xrt_core::hwctx_handle*>(context);
    auto hwctx_obj = dynamic_cast<shim_xdna_edge::xdna_hwctx*>(hwctx_hdl);
    auto aieArray = hwctx_obj->get_aie_array();
    return aieArray->get_dev() ;
  }

  static void* allocateAieDevice(void* devHandle)
  {
    auto aieDevInst = static_cast<XAie_DevInst*>(fetchAieDevInst(devHandle)) ;
    if (!aieDevInst)
      return nullptr;
    return new xaiefal::XAieDev(aieDevInst, false) ;
  }

  static void deallocateAieDevice(void* aieDevice)
  {
    auto object = static_cast<xaiefal::XAieDev*>(aieDevice) ;
    if (object != nullptr)
      delete object ;
  }
} // end anonymous namespace

namespace xdp {
  using tile_type = xdp::tile_type;
  using module_type = xdp::module_type;
  using severity_level = xrt_core::message::severity_level;

  AieDtrace_VE2Impl::AieDtrace_VE2Impl(VPDatabase* database, std::shared_ptr<AieProfileMetadata> metadata, uint64_t deviceID)
      : AieProfileImpl(database, metadata, deviceID)
  {
    auto hwGen = metadata->getHardwareGen();

    coreStartEvents = aie::profile::getCoreEventSets(hwGen);
    coreEndEvents = coreStartEvents;

    memoryStartEvents = aie::profile::getMemoryEventSets(hwGen);
    memoryEndEvents = memoryStartEvents;

    shimStartEvents = aie::profile::getInterfaceTileEventSets(hwGen);
    for (const auto& kv : aie::dtrace::getBandwidthInterfaceTileEventSets(hwGen))
      shimStartEvents[kv.first] = kv.second;
    shimEndEvents = shimStartEvents;
    shimEndEvents[METRIC_BYTE_COUNT] = {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PERF_CNT_0_PL};

    memTileStartEvents = aie::profile::getMemoryTileEventSets(hwGen);
    memTileEndEvents = memTileStartEvents;
    
    microcontrollerEvents = aie::profile::getMicrocontrollerEventSets(hwGen);
  }

  AieDtrace_VE2Impl::~AieDtrace_VE2Impl()
  {
    // Hw context destructor / plugin teardown: do not read counters or offload samples to the
    // dynamic database. Only stop/release FAL resources; join if a poll thread existed.
    if (thread && thread->joinable()) {
      threadCtrl = false;
      thread->join();
    }
    releaseConfiguredHwResourcesNoRead();
  }

  bool AieDtrace_VE2Impl::checkAieDevice(const uint64_t deviceId, void* handle)
  {
    aieDevInst = static_cast<XAie_DevInst*>(db->getStaticInfo().getAieDevInst(fetchAieDevInst, handle, deviceId)) ;
    aieDevice  = static_cast<xaiefal::XAieDev*>(db->getStaticInfo().getAieDevice(allocateAieDevice, deallocateAieDevice, handle, deviceId)) ;
    if (!aieDevInst || !aieDevice) {
      xrt_core::message::send(severity_level::warning, "XRT", 
          "Unable to get AIE device. There will be no AIE profiling.");
      return false;
    }
    return true;
  }

  void AieDtrace_VE2Impl::updateDevice() {

      if(!checkAieDevice(deviceID, metadata->getHandle()))
              return;

      // CT file handles all hardware configuration via write_reg commands in begin block.
      // No need to submit nop.elf or call setMetricsSettings here.
      // The code below is preserved for potential fallback flow in the future.
      return;

      bool runtimeCounters = setMetricsSettings(deviceID, metadata->getHandle());

      if (!runtimeCounters) {
        void* h = metadata->getHandle();
        std::shared_ptr<xrt_core::device> device = xrt_core::get_userpf_device(h);
        if (!device)
          device = xdp::util::convertToCoreDevice(h, true);
        if (!device) {
          xrt_core::message::send(severity_level::warning, "XRT",
              "AIE dtrace: could not resolve core device for xclbin profile counters.");
          (db->getStaticInfo()).setIsAIECounterRead(deviceID, true);
          return;
        }
        auto counters = xrt_core::edge::aie::get_profile_counters(device.get());

        if (counters.empty()) {
          xrt_core::message::send(severity_level::warning, "XRT", 
            "AIE dtrace: no counters found. Specify "
            "AIE_dtrace_settings.tile_based_interface_tile_metrics or "
            "graph_based_interface_tile_metrics (same style as AIE_profile_settings).");
          (db->getStaticInfo()).setIsAIECounterRead(deviceID,true);
          return;
        }
        else {
          XAie_DevInst* aieDevInst =
            static_cast<XAie_DevInst*>(db->getStaticInfo().getAieDevInst(fetchAieDevInst, metadata->getHandle()));
          
          if (!aieDevInst) {
            xrt_core::message::send(severity_level::warning, "XRT", 
              "Failed to get AIE device instance for profile counters.");
            return;
          }

          xrt_core::message::send(severity_level::debug, "XRT", "Processing " + std::to_string(counters.size()) + " counters");
          for (auto& counter : counters) {
            std::stringstream msg;
            msg << "Adding counter " << counter.id << " at (" 
                << +counter.column << "," << +counter.row << ") module: " << counter.module;
            xrt_core::message::send(severity_level::debug, "XRT", msg.str());
            
            // For pre-configured counters from xclbin metadata, the hardware is already configured
            // Payload is used for reporting metadata (channel/stream IDs), set to 0 for these counters
            // as we don't have full tile information (stream_ids, is_master_vec) to safely compute it
            uint64_t payload = 0;
            
            (db->getStaticInfo()).addAIECounter(deviceID, counter.id, counter.column,
                counter.row, counter.counterNumber, counter.startEvent, counter.endEvent,
                counter.resetEvent, payload, counter.clockFreqMhz, counter.module, counter.name);
          }
          xrt_core::message::send(severity_level::debug, "XRT", "Finished processing counters");
        }
      }
  }

  void AieDtrace_VE2Impl::computeOpLocations(void* elf_handle, const std::string& kernel_name)
  {
    if (m_op_locations_cache.count(kernel_name))
      return;

    if (!elf_handle) {
      xrt_core::message::send(severity_level::debug, "XRT",
          "AIE dtrace: No ELF handle available for kernel '" + kernel_name + "'");
      return;
    }

    try {
      auto buf = xdp::get_elf_buffer(elf_handle);
      aiebu::aiebu_assembler assembler(buf);

      auto get_op_tbl = [&]() {
        if (!kernel_name.empty()) {
          try {
            return assembler.get_op_locations(0x1c, kernel_name);
          }
          catch (...) {
            xrt_core::message::send(severity_level::debug, "XRT",
                "AIE dtrace: get_op_locations with kernel name '" + kernel_name
                + "' failed, retrying without kernel name");
          }
        }
        return assembler.get_op_locations(0x1c);
      };

      m_op_locations_cache[kernel_name] = get_op_tbl().get_line_info();

      std::stringstream msg;
      msg << "AIE dtrace: Extracted " << m_op_locations_cache[kernel_name].size()
          << " instance op_locations for kernel '" << kernel_name << "' from ELF";
      xrt_core::message::send(severity_level::debug, "XRT", msg.str());
    }
    catch (const std::exception& e) {
      std::stringstream msg;
      msg << "AIE dtrace: Could not extract op_locations from ELF for kernel '"
          << kernel_name << "': " << e.what();
      xrt_core::message::send(severity_level::debug, "XRT", msg.str());
    }
  }

  void AieDtrace_VE2Impl::generateCTForRun(void* run, void* hwctx, uint32_t run_uid,
                                             const std::string& kernel_name,
                                             void* elf_handle)
  {
    if (!xrt_core::config::get_aie_dtrace())
      return;

    auto ctx = xrt_core::hw_context_int::create_hw_context_from_implementation(hwctx);
    auto slotIdx = static_cast<xrt_core::hwctx_handle*>(ctx)->get_slotidx();

    std::string filename = "aie_profile_ctx_" + std::to_string(slotIdx)
                         + "_run_" + std::to_string(run_uid) + ".ct";
    std::string outputPath = (std::filesystem::current_path() / filename).string();

    computeOpLocations(elf_handle, kernel_name);

    boost::property_tree::ptree aiePartitionPt = xdp::aie::getAIEPartitionInfo(hwctx);
    uint8_t partitionStartCol = aiePartitionPt.empty() ? 0
        : static_cast<uint8_t>(aiePartitionPt.back().second.get<uint64_t>("start_col"));

    AieDtraceCTWriter ctWriter(db, metadata, deviceID, partitionStartCol);

    bool generated = false;
    auto it = m_op_locations_cache.find(kernel_name);

    if (it != m_op_locations_cache.end() && !it->second.empty()) {
      generated = ctWriter.generateBandwidthCT(outputPath, hwctx, it->second);
      if (generated) {
        xrt_core::message::send(severity_level::debug, "XRT",
            "AIE dtrace: Bandwidth CT generated (self-contained) for kernel '"
            + kernel_name + "'");
      }
    }

    if (!generated && it != m_op_locations_cache.end() && !it->second.empty()) {
      if (db->getStaticInfo().getNumAIECounter(deviceID) > 0) {
        generated = ctWriter.generate(outputPath, it->second);
        if (generated)
          xrt_core::message::send(severity_level::debug, "XRT",
              "AIE dtrace: CT generated using aiebu API (get_op_locations) for kernel '"
              + kernel_name + "'");
      }
    }

    if (!generated && db->getStaticInfo().getNumAIECounter(deviceID) > 0) {
      generated = ctWriter.generate(outputPath);
      if (generated)
        xrt_core::message::send(severity_level::debug, "XRT",
            "AIE dtrace: CT generated using CSV file (aie_profile_timestamps.csv) for kernel '"
            + kernel_name + "'");
    }

    if (!generated)
      return;

    auto* xrt_run = static_cast<xrt::run*>(run);
    try {
      xrt_run->set_dtrace_control_file(outputPath);
      std::stringstream msg;
      msg << "AIE dtrace: Set per-run CT file '" << outputPath
          << "' for run uid=" << run_uid << " ctx slot=" << slotIdx;
      xrt_core::message::send(severity_level::info, "XRT", msg.str());
    }
    catch (const std::exception& e) {
      std::stringstream msg;
      msg << "AIE dtrace: Could not set per-run CT file: " << e.what();
      xrt_core::message::send(severity_level::debug, "XRT", msg.str());
    }
  }

  // Get reportable payload specific for this tile and/or counter
  uint64_t 
  AieDtrace_VE2Impl::getCounterPayload(XAie_DevInst* aieDevInst, 
                                         const tile_type& tile, 
                                         const module_type type, 
                                         uint8_t column, 
                                         uint8_t row, 
                                         uint16_t startEvent, 
                                         const std::string metricSet,
                                         const uint8_t channel,
                                         uint8_t logicalPortIndex)
  {
    // 1. Profile API specific values
    if (aie::profile::profileAPIMetricSet(metricSet))
      return getAdfProfileAPIPayload(tile, metricSet);
    
    // 2. Channel/stream IDs for interface tiles
    if (type == module_type::shim) {
      // NOTE: value = ((isMaster) << 8) & (isChannel << 7) & (channel/stream ID)
      // portnum = physical stream-switch port (0-7) from event; stream_ids/is_master_vec
      // are indexed by logical port (size = number of configured ports). When portnum is
      // out of range (e.g. physical ports 4-7 when only 4 logical ports), use
      // logicalPortIndex.
      auto portnum = xdp::aie::getPortNumberFromEvent(static_cast<XAie_Events>(startEvent));
      uint8_t streamPortId = (portnum >= tile.stream_ids.size()) ?
          0 : static_cast<uint8_t>(tile.stream_ids.at(portnum));
      uint8_t idToReport = (tile.subtype == io_type::GMIO) ? channel : streamPortId;
      uint8_t isChannel  = (tile.subtype == io_type::GMIO) ? 1 : 0;
      uint8_t isMaster = aie::isInputSet(type, metricSet)  ? 0 : 1;
      if ((type == module_type::shim) && ((metricSet == "ddr_bandwidth") || (metricSet == "read_bandwidth") || (metricSet == "write_bandwidth"))) {
        uint8_t idx = (portnum < tile.is_master_vec.size()) ? portnum
                    : (logicalPortIndex < tile.is_master_vec.size()) ? logicalPortIndex : 0;
        isMaster = tile.is_master_vec.at(idx);
      }

      return ((isMaster << PAYLOAD_IS_MASTER_SHIFT)
             | (isChannel << PAYLOAD_IS_CHANNEL_SHIFT) | idToReport);
    }

    // 3. Channel IDs for memory tiles
    if (type == module_type::mem_tile) {
      // NOTE: value = ((isMaster) << 8) & (isChannel << 7) & (channel ID)
      uint8_t isChannel = 1;
      uint8_t isMaster = aie::isInputSet(type, metricSet) ? 1 : 0;
      return ((isMaster << PAYLOAD_IS_MASTER_SHIFT) 
             | (isChannel << PAYLOAD_IS_CHANNEL_SHIFT) | channel);
    }

    // 4. DMA BD sizes for AIE tiles
    // NOTE: value = ((max BD size) << 16) & ((isMaster) << 8) & (isChannel << 7) & (channel ID)
    uint8_t isChannel = 1;
    uint8_t isMaster  = aie::isInputSet(type, metricSet) ? 1 : 0;
    uint32_t payloadValue = ((isMaster << PAYLOAD_IS_MASTER_SHIFT) 
                            | (isChannel << PAYLOAD_IS_CHANNEL_SHIFT) | channel);

    if ((metadata->getHardwareGen() != 1)
        || ((startEvent != XAIE_EVENT_DMA_S2MM_0_FINISHED_BD_MEM)
        && (startEvent != XAIE_EVENT_DMA_S2MM_1_FINISHED_BD_MEM)
        && (startEvent != XAIE_EVENT_DMA_MM2S_0_FINISHED_BD_MEM)
        && (startEvent != XAIE_EVENT_DMA_MM2S_1_FINISHED_BD_MEM)))
      return payloadValue;

    // Get average BD size for throughput calculations (AIE1 only)
    constexpr int NUM_BDS = 8;
    constexpr uint32_t BYTES_PER_WORD = 4;
    constexpr uint32_t ACTUAL_OFFSET = 1;
    uint64_t offsets[NUM_BDS] = {XAIEGBL_MEM_DMABD0CTRL,            XAIEGBL_MEM_DMABD1CTRL,
                                 XAIEGBL_MEM_DMABD2CTRL,            XAIEGBL_MEM_DMABD3CTRL,
                                 XAIEGBL_MEM_DMABD4CTRL,            XAIEGBL_MEM_DMABD5CTRL,
                                 XAIEGBL_MEM_DMABD6CTRL,            XAIEGBL_MEM_DMABD7CTRL};
    uint32_t lsbs[NUM_BDS]    = {XAIEGBL_MEM_DMABD0CTRL_LEN_LSB,    XAIEGBL_MEM_DMABD1CTRL_LEN_LSB,
                                 XAIEGBL_MEM_DMABD2CTRL_LEN_LSB,    XAIEGBL_MEM_DMABD3CTRL_LEN_LSB,
                                 XAIEGBL_MEM_DMABD4CTRL_LEN_LSB,    XAIEGBL_MEM_DMABD5CTRL_LEN_LSB,
                                 XAIEGBL_MEM_DMABD6CTRL_LEN_LSB,    XAIEGBL_MEM_DMABD7CTRL_LEN_LSB};
    uint32_t masks[NUM_BDS]   = {XAIEGBL_MEM_DMABD0CTRL_LEN_MASK,   XAIEGBL_MEM_DMABD1CTRL_LEN_MASK,
                                 XAIEGBL_MEM_DMABD2CTRL_LEN_MASK,   XAIEGBL_MEM_DMABD3CTRL_LEN_MASK,
                                 XAIEGBL_MEM_DMABD4CTRL_LEN_MASK,   XAIEGBL_MEM_DMABD5CTRL_LEN_MASK,
                                 XAIEGBL_MEM_DMABD6CTRL_LEN_MASK,   XAIEGBL_MEM_DMABD7CTRL_LEN_MASK};
    uint32_t valids[NUM_BDS]  = {XAIEGBL_MEM_DMABD0CTRL_VALBD_MASK, XAIEGBL_MEM_DMABD1CTRL_VALBD_MASK,
                                 XAIEGBL_MEM_DMABD2CTRL_VALBD_MASK, XAIEGBL_MEM_DMABD3CTRL_VALBD_MASK,
                                 XAIEGBL_MEM_DMABD4CTRL_VALBD_MASK, XAIEGBL_MEM_DMABD5CTRL_VALBD_MASK,
                                 XAIEGBL_MEM_DMABD6CTRL_VALBD_MASK, XAIEGBL_MEM_DMABD7CTRL_VALBD_MASK};

    uint32_t maxBDSize = 0;
    auto tileOffset = XAie_GetTileAddr(aieDevInst, row, column);
    for (int bd = 0; bd < NUM_BDS; ++bd) {
      uint32_t regValue = 0;
      XAie_Read32(aieDevInst, tileOffset + offsets[bd], &regValue);
      
      if (regValue & valids[bd]) {
        uint32_t bdBytes = BYTES_PER_WORD * (((regValue >> lsbs[bd]) & masks[bd]) + ACTUAL_OFFSET);
        maxBDSize = std::max(bdBytes, maxBDSize);
      }
    }

    payloadValue |= (maxBDSize << PAYLOAD_BD_SIZE_SHIFT);
    return payloadValue;
  }
  
  uint64_t 
  AieDtrace_VE2Impl::getAdfProfileAPIPayload(const tile_type& tile, const std::string metricSet)
  {
    if (metricSet == METRIC_LATENCY)
      return metadata->getIntfLatencyPayload(tile);

    return 0;
  }

  void AieDtrace_VE2Impl::printTileModStats(xaiefal::XAieDev* aieDevice, 
      const tile_type& tile, XAie_ModuleType mod)
  {
    auto col = tile.col;
    auto row = tile.row;
    auto loc = XAie_TileLoc(col, row);
    std::string moduleName = (mod == XAIE_CORE_MOD) ? "aie" 
                           : ((mod == XAIE_MEM_MOD) ? "aie_memory" 
                           : "interface_tile");
    const std::string groups[3] = {
      XAIEDEV_DEFAULT_GROUP_GENERIC,
      XAIEDEV_DEFAULT_GROUP_STATIC,
      XAIEDEV_DEFAULT_GROUP_AVAIL
    };

    std::stringstream msg;
    msg << "Resource usage stats for Tile : (" << +col << "," << +row 
        << ") Module : " << moduleName << std::endl;
    for (auto&g : groups) {
      auto stats = aieDevice->getRscStat(g);
      auto pc = stats.getNumRsc(loc, mod, xaiefal::XAIE_PERFCOUNT);
      auto ts = stats.getNumRsc(loc, mod, xaiefal::XAIE_TRACEEVENT);
      auto bc = stats.getNumRsc(loc, mod, xaiefal::XAIE_BROADCAST);
      msg << "Resource Group : " << std::left <<  std::setw(10) << g << " "
          << "Performance Counters : " << pc << " "
          << "Trace Slots : " << ts << " "
          << "Broadcast Channels : " << bc << " "
          << std::endl;
    }

    xrt_core::message::send(severity_level::info, "XRT", msg.str());
  }

  // Set metrics for all specified AIE counters on this device with configs given in AIE_profile_settings
  bool 
  AieDtrace_VE2Impl::setMetricsSettings(const uint64_t deviceId, void* handle)
  {
    int counterId = 0;
    bool runtimeCounters = false;

    auto stats = aieDevice->getRscStat(XAIEDEV_DEFAULT_GROUP_AVAIL);
    auto hwGen = metadata->getHardwareGen();
    auto configChannel0 = metadata->getConfigChannel0();
    auto configChannel1 = metadata->getConfigChannel1();
    uint8_t startColShift = metadata->getPartitionOverlayStartCols().front();
    aie::displayColShiftInfo(startColShift);

    for (int module = 0; module < metadata->getNumModules(); ++module) {
      auto configMetrics = metadata->getConfigMetricsVec(module);
      if (configMetrics.empty())
        continue;
      
      int numTileCounters[metadata->getNumCountersMod(module)+1] = {0};
      XAie_ModuleType mod = aie::profile::getFalModuleType(module);
      
      // Iterate over tiles and metrics to configure all desired counters
      for (auto& tileMetric : configMetrics) {
        auto& metricSet  = tileMetric.second;
        auto tile        = tileMetric.first;
        auto col         = tile.col + startColShift;
        auto row         = tile.row;
        auto subtype     = tile.subtype;
        auto type        = aie::getModuleType(row, metadata->getAIETileRowOffset());
        if ((mod == XAIE_MEM_MOD) && (type == module_type::core))
          type = module_type::dma;
        
        // Catch microcontroller event sets for MDM
        if (module == static_cast<int>(module_type::uc)) {
          // Configure
          auto events = microcontrollerEvents[metricSet];
          aie::profile::configMDMCounters(aieDevInst, hwGen, col, row, events);
          // Record
          tile_type recordTile;
          recordTile.col = col;
          recordTile.row = row;
          microcontrollerTileEvents[recordTile] = events;
          runtimeCounters = true;
          continue;
        }

        // Ignore invalid types and inactive modules
        // NOTE: Inactive core modules are configured when utilizing
        //       stream switch monitor ports to profile DMA channels
        if (!aie::profile::isValidType(type, mod))
          continue;
        if ((type == module_type::dma) && !tile.active_memory)
          continue;
        if ((type == module_type::core) && !tile.active_core) {
          if (metadata->getPairModuleIndex(metricSet, type) < 0)
            continue;
        }

        // Skip interface tiles with empty stream_ids for throughput metrics
        if ((type == module_type::shim) && 
            ((metricSet == "read_bandwidth") || (metricSet == "write_bandwidth") || (metricSet == "ddr_bandwidth")) &&
            tile.stream_ids.empty()) {
          std::stringstream msg;
          msg << "Skipping " << metricSet << " configuration for tile (" << +col << "," << +row 
              << ") - stream_ids is empty";
          xrt_core::message::send(severity_level::warning, "XRT", msg.str());
          continue;
        }

        auto loc         = XAie_TileLoc(col, row);
        auto& xaieTile   = aieDevice->tile(col, row);
        auto xaieModule  = (mod == XAIE_CORE_MOD) ? xaieTile.core()
                         : ((mod == XAIE_MEM_MOD) ? xaieTile.mem() 
                         : xaieTile.pl());

        auto startEvents = (type  == module_type::core) ? coreStartEvents[metricSet]
                         : ((type == module_type::dma)  ? memoryStartEvents[metricSet]
                         : ((type == module_type::shim) ? shimStartEvents[metricSet]
                         : memTileStartEvents[metricSet]));
        auto endEvents   = (type  == module_type::core) ? coreEndEvents[metricSet]
                         : ((type == module_type::dma)  ? memoryEndEvents[metricSet]
                         : ((type == module_type::shim) ? shimEndEvents[metricSet]
                         : memTileEndEvents[metricSet]));
        std::vector<XAie_Events> resetEvents = {};

        int numCounters  = 0;
        auto numFreeCtr  = stats.getNumRsc(loc, mod, xaiefal::XAIE_PERFCOUNT);
        
        if (aie::isDebugVerbosity() && ((metricSet == "ddr_bandwidth") || (metricSet == "read_bandwidth") || (metricSet == "write_bandwidth"))) {
          std::stringstream msg;
          msg << metricSet << " **** counter reservation: tile (" << +col << "," << +row 
              << ") startEvents.size()=" << startEvents.size()
              << " hardware_counters=" << numFreeCtr
              << " tile.stream_ids.size()=" << tile.stream_ids.size();
          xrt_core::message::send(severity_level::debug, "XRT", msg.str());
        }
        
        numFreeCtr = (startEvents.size() < numFreeCtr) ? startEvents.size() : numFreeCtr;
        if ((type == module_type::shim) && ((metricSet == "ddr_bandwidth") || (metricSet == "read_bandwidth") || (metricSet == "write_bandwidth"))) {
          numFreeCtr = tile.stream_ids.size();
        }

        int numFreeCtrSS = numFreeCtr;
        if (aie::profile::profileAPIMetricSet(metricSet)) {
          if (numFreeCtr < 2) {
            continue;
          }
          // We need to monitor single stream switch monitor port
          // numFreeCtrSS = 1 ;
        }

        // Specify Sel0/Sel1 for memory tile events 21-44
        auto iter0 = configChannel0.find(tile);
        auto iter1 = configChannel1.find(tile);
        uint8_t channel0 = (iter0 == configChannel0.end()) ? 0 : iter0->second;
        uint8_t channel1 = (iter1 == configChannel1.end()) ? 1 : iter1->second;
        
        // Modify events as needed
        aie::profile::modifyEvents(type, subtype, channel0, startEvents, metadata->getHardwareGen());
        endEvents = startEvents;

        // TBD : Placeholder to configure AIE core with required profile counters.
        aie::profile::configEventSelections(aieDevInst, loc, type, metricSet, channel0);
        // TBD : Placeholder to configure shim tile with required profile counters.

        aie::profile::configStreamSwitchPorts(tileMetric.first, xaieTile, loc, type, 
            numFreeCtrSS, metricSet, channel0, channel1, startEvents, endEvents, streamPorts);
       
        // Identify the profiling API metric sets and configure graph events
        if (metadata->getUseGraphIterator() && !graphItrBroadcastConfigDone) {
          XAie_Events bcEvent = XAIE_EVENT_NONE_CORE;
          bool status = aie::profile::configGraphIteratorAndBroadcast(aieDevInst, aieDevice,
              metadata, xaieModule, loc, mod, type, metricSet, bcEvent, bcResourcesBytesTx);
          if (status) {
            graphIteratorBrodcastChannelEvent = bcEvent;
            graphItrBroadcastConfigDone = true;
          }
        }

        if (aie::profile::profileAPIMetricSet(metricSet)) {
          // Re-use the existing port running event for both the counters
          startEvents[startEvents.size()-1] = startEvents[0];
          
          // Use start events as End events for profile counters if threshold is not provided
          endEvents[endEvents.size()-1] = endEvents[0];

          // Use the set values broadcast events for the reset of counter
          resetEvents = {XAIE_EVENT_NONE_CORE, XAIE_EVENT_NONE_CORE};
          if (type == module_type::shim) {
            if (metadata->getUseGraphIterator())
              resetEvents = {graphIteratorBrodcastChannelEvent, graphIteratorBrodcastChannelEvent};
            else
              resetEvents = {XAIE_EVENT_NONE_CORE, XAIE_EVENT_USER_EVENT_1_PL};
          }
        }

        uint32_t threshold = 0;
        // Request and configure all available counters for this tile
        for (int i=0; i < numFreeCtr; ++i) {
          auto startEvent    = startEvents.at(i);
          auto endEvent      = endEvents.at(i);
          auto resetEvent    = XAIE_EVENT_NONE_CORE;
          auto portnum       = xdp::aie::getPortNumberFromEvent(startEvent);
          // For metric sets with 4 ports (like ddr_bandwidth), use modulo for channel mapping
          uint8_t channelNum = portnum % 2;
          uint8_t channel    = (channelNum == 0) ? channel0 : channel1;

          // Configure group event before reserving and starting counter
          aie::profile::configGroupEvents(aieDevInst, loc, mod, type, metricSet, startEvent, channel);

          // Configure the profile counters for profile APIs metric sets.
          std::shared_ptr<xaiefal::XAiePerfCounter> perfCounter = nullptr;
          if (aie::profile::profileAPIMetricSet(metricSet)) {
            resetEvent = resetEvents.at(i);
            threshold = metadata->getUserSpecifiedThreshold(tileMetric.first, tileMetric.second);
            threshold = aie::profile::convertToBeats(tileMetric.second, threshold, metadata->getHardwareGen());

            if (i==0 && threshold>0)
              endEvent = XAIE_EVENT_PERF_CNT_1_PL;
              
            if (i==1 && threshold == 0)
              continue;
            
            XAie_Events retCounterEvent = XAIE_EVENT_NONE_CORE;
            perfCounter = aie::profile::configProfileAPICounters(aieDevInst, aieDevice, metadata, xaieModule, 
                            mod, type, metricSet, startEvent, endEvent, resetEvent, i, perfCounters.size(),
                            threshold, retCounterEvent, tile, bcResourcesLatency, adfAPIResourceInfoMap, adfAPIBroadcastEventsMap);
          }
          else {
            // Request counter from resource manager
            perfCounter = xaieModule.perfCounter();
            auto ret = perfCounter->initialize(mod, startEvent, mod, endEvent);
            if (ret != XAIE_OK) break;
            ret = perfCounter->reserve();
            if (ret != XAIE_OK) break;

            // Start the counter
            ret = perfCounter->start();
            if (ret != XAIE_OK) break;
          }
          if (!perfCounter)
            continue;
          perfCounters.push_back(perfCounter);

          // Generate user_event_1 for byte count metric set after configuration
          if ((metricSet == METRIC_BYTE_COUNT) && (i == 1) && !graphItrBroadcastConfigDone) {
            XAie_LocType tileloc = XAie_TileLoc(tile.col, tile.row);
            //Note: For BYTE_COUNT metric, user_event_1 is used twice as eventA & eventB to
            //      to transition the FSM from Idle->State0->State1.
            //      eventC = Port Running and eventD = stop event (counter event).
            XAie_EventGenerate(aieDevInst, tileloc, mod, XAIE_EVENT_USER_EVENT_1_PL);
            XAie_EventGenerate(aieDevInst, tileloc, mod, XAIE_EVENT_USER_EVENT_1_PL);
          }

          // Convert enums to physical event IDs for reporting purposes
          auto physicalEventIds  = aie::profile::getEventPhysicalId(aieDevInst, loc, mod, type, metricSet, 
                                                                    startEvent, endEvent);
          uint16_t phyStartEvent = physicalEventIds.first;
          uint16_t phyEndEvent   = physicalEventIds.second;

          // Get payload for reporting purposes
          uint64_t payload = getCounterPayload(aieDevInst, tileMetric.first, type, col, row, 
                                               startEvent, metricSet, channel, static_cast<uint8_t>(i));
          // Store counter info in database
          std::string counterName = "AIE Counter " + std::to_string(counterId);
          (db->getStaticInfo()).addAIECounter(deviceId, counterId, col, row, i,
                phyStartEvent, phyEndEvent, resetEvent, payload, metadata->getClockFreqMhz(), 
                metadata->getModuleName(module), counterName, (tile.stream_ids.empty() ? 0 : tile.stream_ids[0]));
          counterId++;
          numCounters++;
        } // numFreeCtr

        std::stringstream msg;
        msg << "Reserved " << numCounters << " counters for profiling AIE tile (" << +col 
            << "," << +row << ") using metric set " << metricSet << ".";
        xrt_core::message::send(severity_level::debug, "XRT", msg.str());
        numTileCounters[numCounters]++;
      } // configMetrics
    
      // Report counters reserved per tile
      {
        std::stringstream msg;
        msg << "AIE profile counters reserved in " << metadata->getModuleName(module) << " - ";
        for (int n=0; n <= metadata->getNumCountersMod(module); ++n) {
          if (numTileCounters[n] == 0)
            continue;
          msg << n << ": " << numTileCounters[n] << " tiles, ";
          (db->getStaticInfo()).addAIECounterResources(deviceId, n, numTileCounters[n], module);
        }
        xrt_core::message::send(severity_level::info, "XRT", msg.str().substr(0, msg.str().size()-2));
      }

      runtimeCounters = true;
    } // modules

    return runtimeCounters;
  }

  // Dtrace does not run a profiling poll loop or offload samples; bandwidth is consumed via dtrace.
  void AieDtrace_VE2Impl::startPoll(const uint64_t /*id*/) {}

  void AieDtrace_VE2Impl::continuePoll(const uint64_t /*id*/) {}

  void AieDtrace_VE2Impl::poll(const uint64_t /*id*/) {}

  void AieDtrace_VE2Impl::endPoll()
  {
    // Hw context destructor calls endAIEDtracePoll -> endPollforDevice; avoid reads/offload here.
    // Resource release happens in ~AieDtrace_VE2Impl when the implementation is destroyed.
  }

  void AieDtrace_VE2Impl::freeResources()
  {
    releaseConfiguredHwResourcesNoRead();
  }

  void AieDtrace_VE2Impl::releaseConfiguredHwResourcesNoRead()
  {
    for (auto& c : perfCounters) {
      if (c) {
        c->stop();
        c->release();
      }
    }

    for (auto& c : streamPorts) {
      if (c) {
        c->stop();
        c->release();
      }
    }

    for (auto& bc : bcResourcesBytesTx) {
      if (bc) {
        bc->stop();
        bc->release();
      }
    }

    for (auto& bc : bcResourcesLatency) {
      if (bc) {
        bc->stop();
        bc->release();
      }
    }
  }

  }
