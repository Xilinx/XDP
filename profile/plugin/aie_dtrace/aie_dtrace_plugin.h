// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2022-2025 Advanced Micro Devices, Inc. All rights reserved

#ifndef XDP_AIE_DTRACE_PLUGIN_DOT_H
#define XDP_AIE_DTRACE_PLUGIN_DOT_H

#include "xdp/profile/plugin/aie_profile/aie_profile_impl.h"
#include "xdp/profile/plugin/aie_profile/aie_profile_metadata.h"
#include "xdp/profile/plugin/vp_base/vp_base_plugin.h"

namespace xdp {

  class AieDtracePlugin : public XDPPlugin
  {
  public:
    AieDtracePlugin();
    ~AieDtracePlugin();
    void updateAIEDtraceDevice(void* handle, bool hw_context_flow);
    void endPollforDevice(void* handle);
    void runConstructorHook(void* run, void* hwctx, uint32_t run_uid,
                            const std::string& kernel_name, void* elf_handle);
    void runStartHook(void* run, void* hwctx, uint32_t run_uid, const std::string& kernel_name);
    void runWaitHook(void* run, void* hwctx, uint32_t run_uid, const std::string& kernel_name,
                     int ert_cmd_state);
    static bool alive();
    void broadcast(VPDatabase::MessageType msg, void* blob);

  private:
    void writeAll(bool openNewFiles) override;
    uint64_t getDeviceIDFromHandle(void* handle);
    void endPoll();

    static bool live;
    static bool configuredOnePartition;
    std::map<void*, std::unique_ptr<AieProfileImpl>> handleToAIEDtraceImpl;
  };

} // namespace xdp

#endif
