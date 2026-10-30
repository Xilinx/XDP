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

#ifndef XDP_PLUGIN_AIE_HALT_VE2_IMPL_H
#define XDP_PLUGIN_AIE_HALT_VE2_IMPL_H

#include <memory>
#include <optional>

#include "xdp/config.h"
#include "xdp/profile/plugin/aie_halt/aie_halt_impl.h"

#include "xrt/xrt_hw_context.h"

extern "C" {
#include <aie_codegen.h>
#include <aie_codegen_inc/xaie2psgbl_params.h>
#include "xdp/profile/device/common/ve2/ve2_transaction.h"
}

namespace xdp {

  class AIEHaltVE2Impl : public AIEHaltImpl
  {

    public :
      explicit AIEHaltVE2Impl(VPDatabase* dB);

      ~AIEHaltVE2Impl() override = default;

      AIEHaltVE2Impl(const AIEHaltVE2Impl&) = delete;
      AIEHaltVE2Impl(AIEHaltVE2Impl&&)      = delete;

      AIEHaltVE2Impl& operator=(const AIEHaltVE2Impl&) = delete;
      AIEHaltVE2Impl& operator=(AIEHaltVE2Impl&&)      = delete;

      void updateDevice(void* hwCtxImpl) override;
      void finishflushDevice(void* hwCtxImpl) override;
      bool prepareHalt(void* hwCtxImpl, const std::string& userKernelName,
                       xrt::kernel& haltKernel) override;
      bool scheduleHalt(const xrt::kernel& haltKernel) override;

    private:
      // Primary path: generate the halt control code and open its kernel.
      // Does not start a run.
      bool generateHaltControlCode(void* hwCtxImpl, xrt::kernel& haltKernel);

      // Fallback path: open a prebuilt control code ELF kernel. Does not start a run.
      bool loadHaltElf(xrt::kernel& haltKernel);

      XAie_DevInst aieDevInst = {0};
      std::unique_ptr<xdp::aie::VE2Transaction> tranxHandler;
      // temporarily cache a single halt kernel
      // this won't be needed if a unique halt is generated for every kernel
      std::optional<xrt::kernel> mHaltKernel;
  };

}

#endif
