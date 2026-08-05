// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#ifndef AIE_DTRACE_VE2_H
#define AIE_DTRACE_VE2_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aiebu/aiebu_assembler.h"
#include "core/edge/common/aie_parser.h"
#include "xdp/profile/plugin/aie_dtrace/aie_dtrace_impl.h"
#include "xdp/profile/plugin/aie_dtrace/util/aie_dtrace_util.h"
#include "xaiefal/xaiefal.hpp"

extern "C" {
#include <aie_codegen.h>
#include <aie_codegen_inc/xaiegbl_params.h>
}

namespace xdp {

  class AieDtrace_VE2Impl : public AieDtraceImpl {
    public:
      AieDtrace_VE2Impl(VPDatabase* database, std::shared_ptr<AieDtraceMetadata> metadata, uint64_t deviceID);
      ~AieDtrace_VE2Impl() override = default;

      void updateDevice() override;

      void generateCTForRun(void* run_impl_ptr, void* hwctx, uint32_t run_uid,
                           const std::string& kernel_name,
                           void* elf_handle) override;

    private:
      void computeOpLocations(void* elf_handle, const std::string& kernel_name);

      std::map<std::string, std::vector<aiebu::aiebu_assembler::op_loc>> m_op_locations_cache;

      // compute_io_bound start/stop PCs read once from the PC metadata JSON.
      std::optional<std::pair<uint32_t, uint32_t>> m_computeIoPcs;
      bool m_computeIoPcsResolved = false;
  };

}

#endif
