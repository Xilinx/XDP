// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

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
void aieDtraceRunConstructor(void* run_impl_ptr, void* hwctx, uint32_t run_uid,
                             const char* kernel_name, void* elf_handle);

extern "C"
XDP_PLUGIN_EXPORT
void aieDtraceRunStart(void* run_impl_ptr, void* hwctx, uint32_t run_uid, const char* kernel_name);

extern "C"
XDP_PLUGIN_EXPORT
void aieDtraceRunWait(void* run_impl_ptr, void* hwctx, uint32_t run_uid, const char* kernel_name,
                      int ert_cmd_state);

#endif
