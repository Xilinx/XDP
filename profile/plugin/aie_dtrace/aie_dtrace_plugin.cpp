// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2022-2025 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/aie_dtrace_plugin.h"

#include "core/common/api/hw_context_int.h"
#include "core/common/config_reader.h"
#include "core/common/message.h"
#include "core/common/system.h"
#include "core/include/xrt/experimental/xrt-next.h"

#include "xdp/profile/database/database.h"
#include "xdp/profile/device/utility.h"
#include "xdp/profile/device/xdp_base_device.h"
#include "xdp/profile/plugin/vp_base/info.h"

#if defined(XDP_VE2_BUILD)
#include "xdp/profile/plugin/aie_dtrace/ve2/aie_dtrace_ve2.h"
#endif

namespace xdp {
  using severity_level = xrt_core::message::severity_level;

  bool AieDtracePlugin::live = false;
  bool AieDtracePlugin::configuredOnePartition = false;

  AieDtracePlugin::AieDtracePlugin()
    : XDPPlugin()
  {
    AieDtracePlugin::live = true;

    db->registerPlugin(this);
    db->registerInfo(info::aie_dtrace);
    db->getStaticInfo().setAieApplication();
  }

  AieDtracePlugin::~AieDtracePlugin()
  {
    xrt_core::message::send(severity_level::info, "XRT", "Destroying AIE dtrace plugin.");
    AieDtracePlugin::live = false;
    endPoll();

    if (VPDatabase::alive()) {
      db->unregisterPlugin(this);
    }
  }

  bool AieDtracePlugin::alive()
  {
    return AieDtracePlugin::live;
  }

  uint64_t AieDtracePlugin::getDeviceIDFromHandle(void* handle)
  {
    auto itr = handleToAIEDtraceImpl.find(handle);
    if (itr != handleToAIEDtraceImpl.end())
      return itr->second->getDeviceID();

    return (db->getStaticInfo()).getDeviceContextUniqueId(handle);
  }

  void AieDtracePlugin::updateAIEDtraceDevice(void* handle, bool hw_context_flow)
  {
    xrt_core::message::send(severity_level::info, "XRT", "AIE dtrace: update device.");

    if (!xrt_core::config::get_aie_dtrace())
      return;

    if (!handle)
      return;

    if (!((db->getStaticInfo()).continueXDPConfig(hw_context_flow)))
      return;

    if ((xrt_core::config::get_aie_profile_settings_config_one_partition()) &&
        (configuredOnePartition)) {
      xrt_core::message::send(
          severity_level::warning, "XRT",
          "AIE dtrace: a previous partition was already configured; skipping "
          "(config_one_partition=true).");
      return;
    }

    if (hw_context_flow) {
      xrt::hw_context ctx = xrt_core::hw_context_int::create_hw_context_from_implementation(handle);
      if (xrt_core::hw_context_int::get_elf_flow(ctx)) {
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
                                "AIE dtrace is not yet supported for Full ELF flow.");
        return;
      }
    }

    auto device = util::convertToCoreDevice(handle, hw_context_flow);
#if !defined(XRT_X86_BUILD) && !defined(XDP_CLIENT_BUILD)
    if (1 == device->get_device_id() && xrt_core::config::get_xdp_mode() == "xdna") {
      xrt_core::message::send(severity_level::warning, "XRT",
                            "AIE dtrace: unexpected ZOCL device with xdp_mode=xdna; skipping.");
      return;
    }
    else if (0 == device->get_device_id() && xrt_core::config::get_xdp_mode() == "zocl") {
#ifdef XDP_VE2_ZOCL_BUILD
      xrt_core::message::send(severity_level::warning, "XRT",
                            "AIE dtrace: XDNA device with xdp_mode=zocl; skipping.");
      return;
#endif
    }
#endif

    auto deviceID = getDeviceIDFromHandle(handle);

    {
#ifdef XDP_CLIENT_BUILD
      (db->getStaticInfo()).updateDeviceFromCoreDevice(deviceID, device);
      (db->getStaticInfo()).setDeviceName(deviceID, "win_device");
#else
      if ((db->getStaticInfo()).getAppStyle() == xdp::AppStyle::REGISTER_XCLBIN_STYLE)
        (db->getStaticInfo()).updateDeviceFromCoreDeviceHwCtxFlow(deviceID, device, handle,
                                                                  hw_context_flow);
      else
        (db->getStaticInfo()).updateDeviceFromHandle(deviceID, nullptr, handle);
#endif
    }

    if (handleToAIEDtraceImpl.find(handle) != handleToAIEDtraceImpl.end())
#ifndef XDP_CLIENT_BUILD
      handleToAIEDtraceImpl.erase(handle);
#else
      return;
#endif

    auto metadata =
        std::make_shared<AieProfileMetadata>(deviceID, handle, aie_dtrace_ini_metadata_tag{});
    if (metadata->aieMetadataEmpty()) {
      xrt_core::message::send(severity_level::debug, "XRT",
                              "AIE dtrace: no AIE metadata for this xclbin; skipping.");
      return;
    }

    if ((xrt_core::config::get_aie_profile_settings_config_one_partition()) &&
        (metadata->isConfigured()))
      configuredOnePartition = true;

    std::unique_ptr<AieProfileImpl> implementation;
#if defined(XDP_VE2_BUILD)
    implementation = std::make_unique<AieDtrace_VE2Impl>(db, metadata, deviceID);
#else
    xrt_core::message::send(severity_level::warning, "XRT",
                          "AIE dtrace: no implementation for this build; skipping.");
    return;
#endif

    if (!(db->getStaticInfo()).isAIECounterRead(deviceID)) {
      implementation->updateDevice();
      (db->getStaticInfo()).setIsAIECounterRead(deviceID, true);
    }

    (db->getStaticInfo()).saveProfileConfig(metadata->createAIEProfileConfig(), deviceID);

    handleToAIEDtraceImpl[handle] = std::move(implementation);
  }

  void AieDtracePlugin::writeAll(bool /*openNewFiles*/)
  {
    for (const auto& kv : handleToAIEDtraceImpl)
      endPollforDevice(kv.first);

    XDPPlugin::endWrite();
    handleToAIEDtraceImpl.clear();
  }

  void AieDtracePlugin::endPollforDevice(void* handle)
  {
    if (!handle)
      return;

    (db->getStaticInfo()).unregisterPluginFromHwContext(handle);

    auto itr = handleToAIEDtraceImpl.find(handle);
    if (itr == handleToAIEDtraceImpl.end())
      return;

    // Drop implementation without endPoll(): dtrace must not read/offload on hwctx teardown;
    // ~AieDtrace_VE2Impl releases FAL resources only.
    handleToAIEDtraceImpl.erase(itr);
  }

  void AieDtracePlugin::runConstructorHook(void* run, void* hwctx, uint32_t run_uid,
                                           const std::string& kernel_name, void* elf_handle)
  {
    if (!xrt_core::config::get_aie_dtrace())
      return;

    auto itr = handleToAIEDtraceImpl.find(hwctx);
    if (itr == handleToAIEDtraceImpl.end()) {
      xrt_core::message::send(severity_level::debug, "XRT",
                              "AIE dtrace: no implementation for hwctx in runConstructorHook");
      return;
    }
    itr->second->generateCTForRun(run, hwctx, run_uid, kernel_name, elf_handle);
  }

  void AieDtracePlugin::runStartHook(void* run, void* hwctx, uint32_t run_uid,
                                     const std::string& kernel_name)
  {
    if (!xrt_core::config::get_aie_dtrace())
      return;

    (void)run;
    (void)hwctx;
    (void)run_uid;
    (void)kernel_name;
  }

  void AieDtracePlugin::runWaitHook(void* run, void* hwctx, uint32_t run_uid,
                                    const std::string& kernel_name, int ert_cmd_state)
  {
    if (!xrt_core::config::get_aie_dtrace())
      return;

    (void)run;
    (void)hwctx;
    (void)run_uid;
    (void)kernel_name;
    (void)ert_cmd_state;
  }

  void AieDtracePlugin::endPoll()
  {
    // Destroy implementations directly; no counter read or sample offload in teardown.
    handleToAIEDtraceImpl.clear();
  }

  void AieDtracePlugin::broadcast(VPDatabase::MessageType msg, void* /*blob*/)
  {
    (void)msg;
  }

} // namespace xdp
