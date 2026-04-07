/**
 * Copyright (C) 2022-2023 Advanced Micro Devices, Inc. - All rights reserved
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

  static void aieDtraceRunConstructor(void* run, void* hwctx, uint32_t run_uid,
                                       const char* kernel_name, void* elf_handle)
  {
    if (AieDtracePlugin::alive())
      aieDtracePluginInstance.runConstructorHook(run, hwctx, run_uid,
                                                 kernel_name ? kernel_name : "",
                                                 elf_handle);
  }

  static void aieDtraceRunStart(void* run, void* hwctx, uint32_t run_uid, const char* kernel_name)
  {
    if (AieDtracePlugin::alive())
      aieDtracePluginInstance.runStartHook(run, hwctx, run_uid,
                                           kernel_name ? kernel_name : "");
  }

  static void aieDtraceRunWait(void* run, void* hwctx, uint32_t run_uid, const char* kernel_name,
                               int ert_cmd_state)
  {
    if (AieDtracePlugin::alive())
      aieDtracePluginInstance.runWaitHook(run, hwctx, run_uid,
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
void aieDtraceRunConstructor(void* run, void* hwctx, uint32_t run_uid,
                              const char* kernel_name, void* elf_handle)
{
  xdp::aieDtraceRunConstructor(run, hwctx, run_uid, kernel_name, elf_handle);
}

extern "C"
void aieDtraceRunStart(void* run, void* hwctx, uint32_t run_uid, const char* kernel_name)
{
  xdp::aieDtraceRunStart(run, hwctx, run_uid, kernel_name);
}

extern "C"
void aieDtraceRunWait(void* run, void* hwctx, uint32_t run_uid, const char* kernel_name,
                      int ert_cmd_state)
{
  xdp::aieDtraceRunWait(run, hwctx, run_uid, kernel_name, ert_cmd_state);
}
