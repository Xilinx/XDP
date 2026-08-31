// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2024-2025 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include<regex>
#include<string>
#include<cassert>
#include<sstream>

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

  void AIEHaltPlugin::updateDevice(void* hwCtxImpl)
  {
#if defined(XDP_CLIENT_BUILD) || defined(XDP_VE2_BUILD)
    if (mHwCtxImpl) {
      // Only 1 device and xclbin is supported now.
      return;
    }

#if defined(XDP_VE2_BUILD)
    const std::string deviceName = "ve2_device";
#else
    const std::string deviceName = "win_device";
#endif

    xrt::hw_context hwContext = xrt_core::hw_context_int::create_hw_context_from_implementation(hwCtxImpl);
    std::shared_ptr<xrt_core::device> coreDevice = xrt_core::hw_context_int::get_core_device(hwContext);

    xrt_core::message::send(xrt_core::message::severity_level::info, "XRT", "Identify flow type");

    bool isFullELFFlow = false;
    try {
      isFullELFFlow = xrt_core::hw_context_int::get_elf_flow(hwContext);
    } catch (const std::exception& e) {
      std::stringstream msg;
      msg << e.what() << " AIE Halt cannot be enabled before complete configuration." << std::endl;
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT", msg.str());
      return;
    }

    uint64_t deviceId = 0;
    if (isFullELFFlow) {
      /* For Full ELF flow, AIE metadata is carried in an xrt::elf registered
       * with the HWCtx instead of an xclbin.
       */
      xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT", "In Full ELF flow");

      deviceId = (db->getStaticInfo()).getHwCtxImplUidElf(hwCtxImpl);
      auto elfMap = xrt_core::hw_context_int::get_elf_map(hwContext);
      if (elfMap.empty()) {
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
          "AIE Halt ELF flow: hw_context has no registered ELFs. Skipping ELF flow.");
        return;
      }
      auto elf = util::getAieMetadataElf(elfMap);
      if (!elf)
        return;
      (db->getStaticInfo()).updateDeviceFromCoreDeviceElf(deviceId, coreDevice, std::move(*elf));
    } else {
      // xclbin flow : only one device for the Client/VE2 device flow
      deviceId = db->addDevice(deviceName);
      (db->getStaticInfo()).updateDeviceFromCoreDevice(deviceId, coreDevice, false);
    }
    (db->getStaticInfo()).setDeviceName(deviceId, deviceName);

    mHwCtxImpl = hwCtxImpl;

    DeviceDataEntry.valid = true;
#if defined(XDP_VE2_BUILD)
    DeviceDataEntry.implementation = std::make_unique<AIEHaltVE2Impl>(db);
#else
    DeviceDataEntry.implementation = std::make_unique<AIEHaltClientDevImpl>(db);
#endif
    DeviceDataEntry.implementation->setHwContext(hwContext);
    DeviceDataEntry.implementation->updateDevice(mHwCtxImpl);
#endif
  }

  void AIEHaltPlugin::finishflushDevice(void* hwCtxImpl)
  {
#if defined(XDP_CLIENT_BUILD) || defined(XDP_VE2_BUILD)
    if (!mHwCtxImpl || !DeviceDataEntry.valid) {
      return;
    }

    if (hwCtxImpl != mHwCtxImpl) {
      xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
          "New Hw Context Impl passed in AIE Halt Plugin.");
      return;
    }

    DeviceDataEntry.valid = false;
    DeviceDataEntry.implementation->finishflushDevice(mHwCtxImpl);
#endif
  }

  void AIEHaltPlugin::writeAll(bool /*openNewFiles*/)
  {
#if defined(XDP_CLIENT_BUILD) || defined(XDP_VE2_BUILD)

    if (!mHwCtxImpl || !DeviceDataEntry.valid) {
      return;
    }

    // For client device flow, only 1 device and xclbin is supported now.
    DeviceDataEntry.valid = false;
    DeviceDataEntry.implementation->finishflushDevice(mHwCtxImpl);
#endif
  }

}
