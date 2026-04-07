/**
 * Copyright (C) 2023-2025 Advanced Micro Devices, Inc. - All rights reserved
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

#ifndef XDP_PLUGIN_AIE_DTRACE_CB_H
#define XDP_PLUGIN_AIE_DTRACE_CB_H

#include <cstdint>
#include "xdp/config.h"

extern "C"
XDP_PLUGIN_EXPORT
void updateAIEDtraceDevice(void* handle, bool hw_context_flow);

extern "C"
XDP_PLUGIN_EXPORT
void endAIEDtracePoll(void* handle);

extern "C"
XDP_PLUGIN_EXPORT
void aieDtraceRunConstructor(void* run, void* hwctx, uint32_t run_uid,
                             const char* kernel_name, void* elf_handle);

extern "C"
XDP_PLUGIN_EXPORT
void aieDtraceRunStart(void* run, void* hwctx, uint32_t run_uid, const char* kernel_name);

extern "C"
XDP_PLUGIN_EXPORT
void aieDtraceRunWait(void* run, void* hwctx, uint32_t run_uid, const char* kernel_name,
                      int ert_cmd_state);

#endif
