// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2024-2025 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include<regex>
#include<string>
#include<cassert>
#include<sstream>

#include "core/common/config_reader.h"
#include "core/common/device.h"
#include "core/common/message.h"
#include "core/common/api/hw_context_int.h"
#include "core/include/xrt/experimental/xrt_elf.h"

#include "xdp/profile/plugin/aie_halt/aie_halt_plugin.h"
#include "xdp/profile/plugin/vp_base/info.h"
#include "xdp/profile/device/utility.h"
#include "xdp/profile/plugin/vp_base/utility.h"

#ifdef XDP_CLIENT_BUILD
#include "xdp/profile/plugin/aie_halt/clientDev/aie_halt.h"
#elif defined (XDP_VE2_BUILD)
#include "xdp/profile/plugin/aie_halt/ve2/aie_halt.h"
#include "xdp/profile/device/xdp_base_device.h"
#endif

namespace xdp {

  bool AIEHaltPlugin::live = false;

  AIEHaltPlugin::AIEHaltPlugin()
    : XDPPlugin()
  {
    AIEHaltPlugin::live = true;

    db->registerPlugin(this);
    db->registerInfo(info::aie_halt);
  }

  AIEHaltPlugin::~AIEHaltPlugin()
  {
    if (VPDatabase::alive()) {
      try {
        writeAll(false);
      }
      catch (...) {
      }
      db->unregisterPlugin(this);
    }

    AIEHaltPlugin::live = false;
  }

  bool AIEHaltPlugin::alive()
  {
    return AIEHaltPlugin::live;
  }

  void AIEHaltPlugin::updateDevice(void* /*hwCtxImpl*/)
  {
#if defined(XDP_CLIENT_BUILD) || defined(XDP_VE2_BUILD)
    // updateDevice can run from hw_context::add_config while that context's
    // mutex is held, and an empty context has no handle yet. Halt registration
    // waits until runStartImpl.
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
      "Deferring AIE Halt configuration until run start.");
#endif
  }

  AIEHaltPlugin::ContextHalt*
  AIEHaltPlugin::ensureContext(void* hwCtxImpl)
  {
#if defined(XDP_CLIENT_BUILD) || defined(XDP_VE2_BUILD)
    if (!hwCtxImpl)
      return nullptr;

    auto existing = contextHalts.find(hwCtxImpl);
    if (existing != contextHalts.end())
      return &existing->second;

    xrt::hw_context hwContext =
      xrt_core::hw_context_int::create_hw_context_from_implementation(hwCtxImpl);

    bool isFullELFFlow = false;
    try {
      isFullELFFlow = xrt_core::hw_context_int::get_elf_flow(hwContext);
    } catch (const std::exception& e) {
      std::stringstream msg;
      msg << e.what() << " AIE Halt cannot be enabled before complete configuration.";
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT", msg.str());
      return nullptr;
    }

#if defined(XDP_VE2_BUILD)
    const std::string deviceName = "ve2_device";
#else
    const std::string deviceName = "win_device";
#endif

    std::shared_ptr<xrt_core::device> coreDevice =
      xrt_core::hw_context_int::get_core_device(hwContext);
    uint64_t deviceId = 0;
    if (isFullELFFlow) {
      xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
        "Applying deferred configuration for AIE Halt.");
      deviceId = (db->getStaticInfo()).getHwCtxImplUidElf(hwCtxImpl);
      auto elfMap = xrt_core::hw_context_int::get_elf_map(hwContext);
      if (elfMap.empty()) {
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
          "AIE Halt ELF flow: hw_context has no registered ELFs, halt cannot be configured.");
        return nullptr;
      }
      auto elf = util::getAieMetadataElf(elfMap);
      if (!elf) {
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
          "Metadata not found in ELF map, halt cannot be configured.");
        return nullptr;
      }
      (db->getStaticInfo()).updateDeviceFromCoreDeviceElf(deviceId, coreDevice, std::move(*elf));
    } else {
      deviceId = db->addDevice(deviceName);
      (db->getStaticInfo()).updateDeviceFromCoreDevice(deviceId, coreDevice, false);
    }
    (db->getStaticInfo()).setDeviceName(deviceId, deviceName);

    uint32_t contextId = 0;
    try {
      contextId = xrt_core::hw_context_int::get_slotidx(hwContext);
    } catch (const std::exception& e) {
      std::stringstream msg;
      msg << "AIE Halt cannot read the hardware context id: " << e.what();
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT", msg.str());
      return nullptr;
    }

    ContextHalt context;
#if defined(XDP_VE2_BUILD)
    context.implementation = std::make_unique<AIEHaltVE2Impl>(db);
#else
    context.implementation = std::make_unique<AIEHaltClientDevImpl>(db);
#endif
    context.contextId = contextId;
    context.implementation->setHwContext(std::move(hwContext));

    auto inserted = contextHalts.emplace(hwCtxImpl, std::move(context));
    return &inserted.first->second;
#else
    (void)hwCtxImpl;
    return nullptr;
#endif
  }

  void AIEHaltPlugin::runStartImpl(void* /*run_impl_ptr*/, void* hwCtxImpl,
                                   uint32_t /*run_uid*/,
                                   const std::string& kernel_name)
  {
#if defined(XDP_CLIENT_BUILD) || defined(XDP_VE2_BUILD)
    ContextHalt* context = ensureContext(hwCtxImpl);
    if (!context)
      return;

    // The only live context's first halt. A context created after the previous
    // one has been released is first again. An overlapping second context is not.
    const bool firstEver = contextHalts.size() == 1 && context->haltCount == 0;
    const bool haltEvery = xrt_core::config::get_aie_halt_settings_halt_on_every_run();

    std::stringstream msg;
    msg << "AIE Halt run start for kernel '" << kernel_name
        << "' context_id=" << context->contextId
        << " first_ever=" << firstEver;
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", msg.str());

    if (!(firstEver || haltEvery))
      return;

    const uint32_t haltCount = context->haltCount + 1;
    xrt::kernel haltKernel;
    if (!context->implementation->prepareHalt(hwCtxImpl, kernel_name,
                                              context->contextId, haltCount,
                                              haltKernel)) {
      if (context->haltCount == 0)
        contextHalts.erase(hwCtxImpl);
      return;
    }

    context->haltCount = haltCount;
    context->implementation->scheduleHalt(hwCtxImpl, haltKernel);
#else
    (void)hwCtxImpl;
    (void)kernel_name;
#endif
  }

  void AIEHaltPlugin::finishflushDevice(void* hwCtxImpl)
  {
#if defined(XDP_CLIENT_BUILD) || defined(XDP_VE2_BUILD)
    auto contextIt = contextHalts.find(hwCtxImpl);
    if (contextIt == contextHalts.end())
      return;

    contextIt->second.implementation->finishflushDevice(hwCtxImpl);
    contextHalts.erase(contextIt);
#else
    (void)hwCtxImpl;
#endif
  }

  void AIEHaltPlugin::writeAll(bool /*openNewFiles*/)
  {
#if defined(XDP_CLIENT_BUILD) || defined(XDP_VE2_BUILD)
    for (auto& entry : contextHalts)
      entry.second.implementation->finishflushDevice(entry.first);
    contextHalts.clear();
#endif
  }

}
