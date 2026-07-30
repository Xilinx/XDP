/**
 * Copyright (C) 2025 Advanced Micro Devices, Inc. - All rights reserved
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

#define XDP_PLUGIN_SOURCE

#include <sstream>

#include <boost/property_tree/ptree.hpp>

#include "core/common/api/hw_context_int.h"
#include "core/common/api/xclbin_int.h"
#include "core/common/device.h"
#include "core/common/message.h"

#include "core/include/xclbin.h"
#include "core/include/xrt/experimental/xrt_elf.h"
#include "core/include/xrt/experimental/xrt_ext.h"
#include "core/include/xrt/experimental/xrt_module.h"
#include "core/include/xrt/xrt_kernel.h"

#include "xdp/profile/database/static_info/aie_util.h"
#include "xdp/profile/plugin/aie_halt/ve2/aie_halt.h"
#include "xdp/profile/plugin/vp_base/utility.h"

namespace xdp {

  AIEHaltVE2Impl::AIEHaltVE2Impl(VPDatabase*dB)
    : AIEHaltImpl(dB)
  {
    tranxHandler = std::make_unique<xdp::aie::VE2Transaction>();
  }

  void AIEHaltVE2Impl::updateDevice(void* hwCtxImpl)
  {
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
              "In AIEHaltVE2Impl::updateDevice");

    xrt::hw_context hwContext =
      xrt_core::hw_context_int::create_hw_context_from_implementation(hwCtxImpl);

    // Primary path: generate the halt control code on the fly using aie-codegen.
    if (generateHaltControlCode(hwCtxImpl, hwContext))
      return;

    // Fallback path: load a prebuilt control code ELF from disk.
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
              "Could not generate AIE Halt control code. Falling back to prebuilt control code ELF.");
    loadHaltElf(hwContext);
  }

  bool AIEHaltVE2Impl::generateHaltControlCode(void* hwCtxImpl, xrt::hw_context hwContext)
  {
    using severity_level = xrt_core::message::severity_level;

    // Get the xclbin via the hw context uuid (device->get_xclbin_uuid() reads
    // the sysfs xclbinid file, which is absent on the XDNA path). AIE info is
    // in AIE_TRACE_METADATA, falling back to AIE_METADATA.
    xdp::aie::driver_config meta_config;
    try {
      auto device = xrt_core::hw_context_int::get_core_device(hwContext);
      xrt::xclbin xrtXclbin = device->get_xclbin(hwContext.get_xclbin_uuid());

      auto data = xrt_core::xclbin_int::get_axlf_section(xrtXclbin, AIE_TRACE_METADATA);
      if (!data.first || !data.second)
        data = xrt_core::xclbin_int::get_axlf_section(xrtXclbin, AIE_METADATA);

      if (!data.first || !data.second) {
        xrt_core::message::send(severity_level::warning, "XRT", "Empty AIE Metadata in xclbin");
        return false;
      }

      boost::property_tree::ptree aieMetadata;
      auto metadataReader = xdp::aie::readAIEMetadata(data.first, data.second, aieMetadata);
      if (!metadataReader) {
        xrt_core::message::send(severity_level::warning, "XRT", "Failed to parse AIE Metadata from xclbin.");
        return false;
      }
      meta_config = metadataReader->getDriverConfig();
    } catch (const std::exception& e) {
      std::string msg("AIE Metadata could not be read/processed from xclbin: ");
      msg += e.what();
      xrt_core::message::send(severity_level::warning, "XRT", msg);
      return false;
    }

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
    if (RC != XAIE_OK) {
      xrt_core::message::send(severity_level::warning, "XRT", "AIE Driver Initialization Failed.");
      return false;
    }

    uint64_t startCol = 0, numCols = 0;
    boost::property_tree::ptree aiePartitionPt = xdp::aie::getAIEPartitionInfo(hwCtxImpl);
    for (const auto& e : aiePartitionPt) {
      startCol = e.second.get<uint64_t>("start_col");
      numCols  = e.second.get<uint64_t>("num_cols");
      // Currently, assuming only one Hw Context is alive at a time
      break;
    }

    std::stringstream msg;
    msg << "Set AIE Core breakpoint at Lock Acquire Req Instr, Start col "
        << startCol << ", Num col " << numCols;
    xrt_core::message::send(severity_level::info, "XRT", msg.str());

    // Record driver calls into the control code ASM.
    if (!tranxHandler->initializeTransaction(&aieDevInst, "AieHalt")) {
      xrt_core::message::send(severity_level::warning, "XRT", "AIE Halt transaction initialization failed.");
      return false;
    }

    // Halt each AIE core on the Lock Acquire Request instruction (event 0x2C).
    constexpr uint32_t AIE_EVENT_INSTR_LOCK_ACQ_REQ = 0x2C;
    uint32_t dbg_ctrl_1_reg =
      AIE_EVENT_INSTR_LOCK_ACQ_REQ << XAIE2PSGBL_CORE_MODULE_DEBUG_CONTROL1_DEBUG_HALT_CORE_EVENT0_LSB;

    for (uint8_t c = static_cast<uint8_t>(startCol); c < static_cast<uint8_t>(startCol + numCols); c++) {
      for (uint8_t r = meta_config.aie_tile_row_start; r < meta_config.num_rows; r++) {
        auto tileOffset = XAie_GetTileAddr(&aieDevInst, r, c);
        XAie_Write32(&aieDevInst, tileOffset + XAIE2PSGBL_CORE_MODULE_DEBUG_CONTROL1, dbg_ctrl_1_reg);
      }
    }

    // Assemble the recorded ASM into an ELF and run it on XDP_KERNEL.
    if (!tranxHandler->submitTransaction(&aieDevInst, hwContext)) {
      xrt_core::message::send(severity_level::warning, "XRT",
                "Failed to generate/submit AIE Halt control code.");
      return false;
    }

    xrt_core::message::send(severity_level::info, "XRT",
              "Successfully generated and scheduled AIE Halt control code.");
    return true;
  }

  void AIEHaltVE2Impl::loadHaltElf(xrt::hw_context hwContext)
  {
    std::string inputCtrlCode = xrt_core::config::get_aie_halt_settings_control_code();
    if (inputCtrlCode.empty()) {
      xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
                "No input control code file for AIE Halt provided. Defaulting to \"aieHalt4x4.elf\".");
      inputCtrlCode = "aieHalt4x4.elf";
    }

    xrt::elf haltElf;
    try {
      haltElf = xrt::elf(inputCtrlCode);
    } catch (...) {
      std::string msg = "Failed to load " + inputCtrlCode + ". Cannot configure AIE to halt.";
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT", msg);
      return;
    }

    xrt::module mod{haltElf};
    xrt::kernel krnl;
    try {
      krnl = xrt::ext::kernel{hwContext, mod, "XDP_KERNEL:{IPUV1CNN}"};
    } catch (...) {
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
                "XDP_KERNEL not found in HW Context. Cannot configure AIE to halt.");
      return;
    }

    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
              "In AIEHaltVE2Impl New Kernel Object for XDP_KERNEL created for running control code Elf");

    xrt::run rn{krnl};
    rn.start();
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
              "In AIEHaltVE2Impl run start, going to wait");

    rn.wait2();
  }

  void AIEHaltVE2Impl::finishflushDevice(void* /*hwCtxImpl*/)
  {
  }
}
