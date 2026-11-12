/**
 * Copyright (C) 2024-2025 Advanced Micro Devices, Inc. - All rights reserved
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

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "core/common/device.h"
#include "core/common/message.h"
#include "core/common/api/hw_context_int.h"

#include "xdp/profile/database/database.h"
#include "xdp/profile/database/static_info/aie_util.h"
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/device/common/client_transaction.h"
#include "xdp/profile/plugin/aie_halt/clientDev/aie_halt.h"
#include "xdp/profile/plugin/vp_base/utility.h"

#include "core/common/api/xclbin_int.h"
#include "core/include/xclbin.h"

extern "C" {
#include <aie_codegen.h>
#include <aie_codegen_inc/xaiemlgbl_params.h>
}

#ifdef _WIN32
# pragma warning ( disable : 4244 )
#endif

namespace xdp {


  AIEHaltClientDevImpl::AIEHaltClientDevImpl(VPDatabase*dB)
    : AIEHaltImpl(dB)
  {
  }

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable: 4702)
#endif
  void AIEHaltClientDevImpl::updateDevice(void* /*hwCtxImpl*/)
  {
  }

  bool AIEHaltClientDevImpl::runHalt(void* hwCtxImpl, const std::string& /*userKernelName*/,
                                     uint32_t contextId, uint32_t haltCount)
  {
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
              "In AIEHaltClientDevImpl::runHalt");

    aie::ClientTransaction txnHandler(mHwContext, "AIE Halt");
    if (!txnHandler.initializeKernel("XDP_KERNEL"))
      return false;

    boost::property_tree::ptree aieMetadata;
    try {
      auto device = xrt_core::hw_context_int::get_core_device(mHwContext);
      xrt::xclbin xrtXclbin = device.get()->get_xclbin(device.get()->get_xclbin_uuid());
      auto data = xrt_core::xclbin_int::get_axlf_section(xrtXclbin, AIE_METADATA);

      if (!data.first || !data.second) {
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT", "Empty AIE Metadata in xclbin");
        return false;
      }

      std::stringstream ss;
      ss.write(data.first,data.second);

      boost::property_tree::read_json(ss, aieMetadata);
    } catch (const std::exception& e) {
      std::string msg("AIE Metadata could not be read/processed from xclbin: ");
      msg += e.what();
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT", msg);
      return false;
    }

    xdp::aie::driver_config meta_config = xdp::aie::getDriverConfig(aieMetadata, "aie_metadata.driver_config");

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
      {0}
    };

    XAie_DevInst aieDevInst = {0};
    auto RC = XAie_CfgInitialize(&aieDevInst, &cfg);
    if (RC != XAIE_OK) {
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT", "AIE Driver Initialization Failed.");
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
    msg << " Set AIE Core breakpoint at Lock Acquire Req Instr and reset cores, Start col "
        << startCol << ", Num col " << numCols
        << ", context id " << contextId
        << ", halt count " << haltCount << std::endl;
    xrt_core::message::send(xrt_core::message::severity_level::info, "XRT", msg.str());

    // Initial break on Event 44: Lock Acquire instruction
    constexpr uint32_t AIE_EVENT_INSTR_LOCK_ACQ_REQ = 0x2C;
    uint32_t dbg_ctrl_1_reg = AIE_EVENT_INSTR_LOCK_ACQ_REQ << XAIEMLGBL_CORE_MODULE_DEBUG_CONTROL1_DEBUG_HALT_CORE_EVENT0_LSB;
    XAie_StartTransaction(&aieDevInst, XAIE_TRANSACTION_DISABLE_AUTO_FLUSH);
    for (uint8_t c = static_cast<uint8_t>(startCol) ; c < (static_cast<uint8_t>(startCol + numCols)) ; c++ ) {
      for (uint8_t r = 2; r < 6 ; r++) {
        auto tileOffset = XAie_GetTileAddr(&aieDevInst, r, c);
        XAie_Write32(&aieDevInst, tileOffset + XAIEMLGBL_CORE_MODULE_DEBUG_CONTROL1, dbg_ctrl_1_reg);
        // Hold every core in reset before releasing any of them. A later
        // partition is already parked past main's acquire, and this breakpoint
        // only fires on a new lock-acquire request.
        XAie_MaskWrite32(&aieDevInst, tileOffset + XAIEMLGBL_CORE_MODULE_CORE_CONTROL,
                         XAIEMLGBL_CORE_MODULE_CORE_CONTROL_RESET_MASK,
                         XAIEMLGBL_CORE_MODULE_CORE_CONTROL_RESET_MASK);
      }
    }
    // Cores are held in reset. These values survive the release.
    // Counter 1 is the xrt-smi context id. Counter 2 is this context's halt count.
    for (uint8_t c = static_cast<uint8_t>(startCol) ; c < (static_cast<uint8_t>(startCol + numCols)) ; c++ ) {
      for (uint8_t r = 2; r < 6 ; r++) {
        auto tileOffset = XAie_GetTileAddr(&aieDevInst, r, c);
        XAie_Write32(&aieDevInst, tileOffset + XAIEMLGBL_CORE_MODULE_PERFORMANCE_COUNTER1, contextId);
        XAie_Write32(&aieDevInst, tileOffset + XAIEMLGBL_CORE_MODULE_PERFORMANCE_COUNTER2, haltCount);
      }
    }
    for (uint8_t c = static_cast<uint8_t>(startCol) ; c < (static_cast<uint8_t>(startCol + numCols)) ; c++ ) {
      for (uint8_t r = 2; r < 6 ; r++) {
        auto tileOffset = XAie_GetTileAddr(&aieDevInst, r, c);
        XAie_MaskWrite32(&aieDevInst, tileOffset + XAIEMLGBL_CORE_MODULE_CORE_CONTROL,
                         XAIEMLGBL_CORE_MODULE_CORE_CONTROL_RESET_MASK, 0);
      }
    }

    uint8_t* txnBin = XAie_ExportSerializedTransaction(&aieDevInst, 1, 0);
    if (!txnHandler.submitTransaction(txnBin))
      return false;
    XAie_ClearTransaction(&aieDevInst);
    return true;
  }
#ifdef _WIN32
#pragma warning(pop)
#endif
  void AIEHaltClientDevImpl::finishflushDevice(void* /*hwCtxImpl*/)
  {
  }
}
