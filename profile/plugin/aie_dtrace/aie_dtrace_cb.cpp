// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "aie_dtrace_cb.h"
#include "aie_dtrace_plugin.h"

namespace xdp {

  static AieDtracePlugin aieDtracePluginInstance;

  static void updateAIEDtraceDevice(void* handle, bool hw_context_flow)
  {
    if (AieDtracePlugin::alive())
      aieDtracePluginInstance.updateAIEDtraceDevice(handle, hw_context_flow);
  }

  static void endAIEDtracePoll(void* handle)
  {
    if (AieDtracePlugin::alive())
      aieDtracePluginInstance.endPollforDevice(handle);
  }

  static void aieDtraceRunConstructor(void* run_impl_ptr, void* hwctx, uint32_t run_uid,
                                       const char* kernel_name, void* elf_handle)
  {
    if (AieDtracePlugin::alive())
      aieDtracePluginInstance.runConstructorHook(run_impl_ptr, hwctx, run_uid,
                                                 kernel_name ? kernel_name : "",
                                                 elf_handle);
  }

  static void aieDtraceRunStart(void* run_impl_ptr, void* hwctx, uint32_t run_uid, const char* kernel_name)
  {
    if (AieDtracePlugin::alive())
      aieDtracePluginInstance.runStartHook(run_impl_ptr, hwctx, run_uid,
                                           kernel_name ? kernel_name : "");
  }

  static void aieDtraceRunWait(void* run_impl_ptr, void* hwctx, uint32_t run_uid, const char* kernel_name,
                               int ert_cmd_state)
  {
    if (AieDtracePlugin::alive())
      aieDtracePluginInstance.runWaitHook(run_impl_ptr, hwctx, run_uid,
                                          kernel_name ? kernel_name : "", ert_cmd_state);
  }

} // end namespace xdp

extern "C"
void updateAIEDtraceDevice(void* handle, bool hw_context_flow)
{
  xdp::updateAIEDtraceDevice(handle, hw_context_flow);
}

extern "C"
void endAIEDtracePoll(void* handle)
{
  xdp::endAIEDtracePoll(handle);
}

extern "C"
void aieDtraceRunConstructor(void* run_impl_ptr, void* hwctx, uint32_t run_uid,
                              const char* kernel_name, void* elf_handle)
{
  xdp::aieDtraceRunConstructor(run_impl_ptr, hwctx, run_uid, kernel_name, elf_handle);
}

extern "C"
void aieDtraceRunStart(void* run_impl_ptr, void* hwctx, uint32_t run_uid, const char* kernel_name)
{
  xdp::aieDtraceRunStart(run_impl_ptr, hwctx, run_uid, kernel_name);
}

extern "C"
void aieDtraceRunWait(void* run_impl_ptr, void* hwctx, uint32_t run_uid, const char* kernel_name,
                      int ert_cmd_state)
{
  xdp::aieDtraceRunWait(run_impl_ptr, hwctx, run_uid, kernel_name, ert_cmd_state);
}
