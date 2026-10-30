// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#ifndef AIE_DTRACE_IMPL_H
#define AIE_DTRACE_IMPL_H

#include <memory>
#include <string>

#include "xdp/profile/plugin/aie_dtrace/aie_dtrace_metadata.h"
#include "xdp/profile/plugin/vp_base/vp_base_plugin.h"

namespace xdp {

  // AIE profile configurations can be done in different ways depending
  // on the platform.  For example, platforms like the VCK5000 or
  // discovery platform, where the host code runs on the x86 and the AIE
  // is not directly accessible, will require configuration be done via
  // PS kernel.
  class AieDtraceImpl
  {

  protected:
    VPDatabase* db = nullptr;
    std::shared_ptr<AieDtraceMetadata> metadata;
    uint64_t deviceID;

  public:
    AieDtraceImpl(VPDatabase* database, std::shared_ptr<AieDtraceMetadata> metadata, uint64_t id)
      : db(database),
        metadata(metadata),
        deviceID(id)
    {}

    AieDtraceImpl() = delete;
    virtual ~AieDtraceImpl() {};

    virtual void updateDevice() = 0;

    virtual void startPoll(const uint64_t /*id*/) {}
    virtual void continuePoll(const uint64_t /*id*/) {}
    virtual void poll(const uint64_t /*id*/) {}
    virtual void endPoll() {}
    virtual void freeResources() {}

    // Called once per xrt::run construction, where the ELF is available:
    // generates every CT file the configured inference sequence needs so that
    // run start only has to pick one. When a single metric set covers every
    // inference there is nothing to pick, so the CT is also programmed here.
    virtual void generateCTsForRun(void* /*run_impl_ptr*/, void* /*hwctx*/, uint32_t /*run_uid*/,
                                   const std::string& /*kernel_name*/,
                                   void* /*elf_handle*/) {}

    // Called on every xrt::run start: advances this kernel's inference counter
    // and hands the matching pre-generated CT to XRT. Does nothing unless a
    // multi-inference sequence is configured.
    virtual void applyCTForRun(void* /*run_impl_ptr*/, void* /*hwctx*/, uint32_t /*run_uid*/,
                               const std::string& /*kernel_name*/) {}

    // Called at hardware context teardown, to report a configured inference
    // sequence that the application did not run far enough to consume.
    virtual void reportUnusedSelections() {}

    uint64_t getDeviceID() { return deviceID; }
  };

} // namespace xdp

#endif
