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

#ifndef XDP_PLUGIN_AIE_HALT_CLIENTDEV_IMPL_H
#define XDP_PLUGIN_AIE_HALT_CLIENTDEV_IMPL_H

#include "xdp/config.h"
#include "xdp/profile/plugin/aie_halt/aie_halt_impl.h"

#include <cstdint>

namespace xdp {

  class AIEHaltClientDevImpl : public AIEHaltImpl
  {

    public :
      AIEHaltClientDevImpl(VPDatabase* dB);

      ~AIEHaltClientDevImpl() = default;

      void updateDevice(void* hwCtxImpl) override;
      void finishflushDevice(void* hwCtxImpl) override;
      bool prepareHalt(void* hwCtxImpl, const std::string& userKernelName,
                       uint32_t contextId, uint32_t haltCount,
                       xrt::kernel& haltKernel) override;
      bool scheduleHalt(void* hwCtxImpl, const xrt::kernel& haltKernel) override;

    private:
      // Written into performance counters by the transaction built in scheduleHalt.
      uint32_t mContextId = 0;
      uint32_t mHaltCount = 0;
  };

}

#endif