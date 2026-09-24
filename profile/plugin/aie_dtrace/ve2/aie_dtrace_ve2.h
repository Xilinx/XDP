// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#ifndef AIE_DTRACE_VE2_H
#define AIE_DTRACE_VE2_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
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

      void generateCTsForRun(void* run_impl_ptr, void* hwctx, uint32_t run_uid,
                             const std::string& kernel_name,
                             void* elf_handle) override;

      void applyCTForRun(void* run_impl_ptr, void* hwctx, uint32_t run_uid,
                         const std::string& kernel_name) override;

      void reportUnusedSelections() override;

    private:
      // Callers must hold m_mutex.
      void computeOpLocations(void* elf_handle, const std::string& kernel_name);
      void generateCTFiles(void* hwctx, uint32_t run_uid, const std::string& kernel_name,
                           void* elf_handle);
      const std::vector<std::string>* findCTFiles(const std::string& kernel_name) const;

      // Serializes every mutable member below. One instance exists per hardware
      // context, but XRT lets an application construct and start runs on the
      // same context from several threads.
      mutable std::mutex m_mutex;

      std::map<std::string, std::vector<aiebu::aiebu_assembler::op_loc>> m_op_locations_cache;

      // Kernel name -> one CT path per configured inference, in execution
      // order. An entry is empty when that inference's CT could not be
      // generated. Generated once per kernel at run construction and then
      // shared by every run object of that kernel, since the ELF, and so the
      // SAVE_TIMESTAMPS locations, are identical across them.
      std::map<std::string, std::vector<std::string>> m_ct_files;

      // Kernel name -> inferences started so far on this hardware context.
      // Counted at start rather than at construction because an application is
      // free to reuse one run object for every inference, or to build a pool of
      // run objects up front. Only used for a multi-inference sequence; a
      // single metric set is programmed at construction and never re-selected.
      std::map<std::string, uint64_t> m_inference_counts;
  };

}

#endif
