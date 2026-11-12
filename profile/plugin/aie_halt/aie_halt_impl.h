/**
 * Copyright (C) 2024 Advanced Micro Devices, Inc. - All rights reserved
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

#ifndef XDP_PLUGIN_AIE_HALT_IMPL_H
#define XDP_PLUGIN_AIE_HALT_IMPL_H

#include "core/include/xrt/xrt_hw_context.h"
#include "core/include/xrt/xrt_kernel.h"

#include <cstdint>
#include <string>

namespace xdp {

  class VPDatabase;

  class AIEHaltImpl
  {
    protected :
      VPDatabase* db = nullptr;
      xrt::hw_context mHwContext;

    public:
      AIEHaltImpl(VPDatabase* dB)
        : db(dB)
      {}

      AIEHaltImpl() = delete;

      virtual ~AIEHaltImpl() {}

      virtual void updateDevice(void*) = 0;
      virtual void finishflushDevice(void*) = 0;

      // Build a new halt kernel for this run. contextId is the xrt-smi slot
      // index. haltCount is how many times this context has halted, including
      // this run. The kernel is not retained.
      virtual bool prepareHalt(void* hwCtxImpl, const std::string& userKernelName,
                               uint32_t contextId, uint32_t haltCount,
                               xrt::kernel& haltKernel) = 0;

      virtual bool scheduleHalt(void* hwCtxImpl, const xrt::kernel& haltKernel) = 0;

      void setHwContext(xrt::hw_context ctx)
      {
        mHwContext = std::move(ctx);
      }
  };

}
#endif