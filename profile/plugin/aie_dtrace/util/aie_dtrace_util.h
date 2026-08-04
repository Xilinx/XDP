// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#ifndef AIE_DTRACE_UTIL_DOT_H
#define AIE_DTRACE_UTIL_DOT_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include <aie_codegen.h>
}

namespace xdp::aie::dtrace {

  // Shim bandwidth metric sets used for Debug.aie_dtrace (not part of standard aie_profile ini).
  std::map<std::string, std::vector<XAie_Events>> getBandwidthInterfaceTileEventSets(int hwGen);

  // Enable JSON dtrace_dump output with coalesced results by default.
  // Must run before XRT creates the first dtrace module (config keys lock on first read).
  void initDtraceOutputConfig();

  // Register per-run bandwidth counter metadata (slot -> run uid -> JSON object string),
  // captured during CT generation. Stored in a process-lifetime registry so it survives
  // XDP plugin teardown; the plugin's static instance may be destroyed before the hw
  // context writes the coalesced JSON dump (static destruction order is not guaranteed).
  void registerCounterMetadata(uint32_t slotIdx, uint32_t runUid, const std::string& metadataJson);

  // Inject the registered counter metadata into the coalesced JSON dtrace dump file(s)
  // (dtrace_dump_ctx_<slot>_*.json) that core writes to the cwd on hw context teardown.
  // Safe to call even after the XDP plugin instance has been destroyed; idempotent
  // (each dump file is injected at most once).
  void injectPendingMetadata();

} // namespace xdp::aie::dtrace

#endif
