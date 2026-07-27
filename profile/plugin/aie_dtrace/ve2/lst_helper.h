// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved
#ifndef XDP_AIE_DTRACE_VE2_LST_HELPER_H
#define XDP_AIE_DTRACE_VE2_LST_HELPER_H

#include <cstdint>
#include <optional>
#include <utility>
#include <string>

namespace xdp {

// Helpers for the compute_io_bound "static/inlined design" path. These parse the
// aiecompiler listing (.lst) and source (.cc) for a core tile, located by searching
// the current working directory (the run directory, where the design's aiecompiler
// Work tree is co-located in the Telluride flow).
//
// tileBase is the RELATIVE-row listing base name (e.g. "0_0"): first core tile in
// column 0, from elfs_metadata[col0/row0].static_elfs.

// True if kernelWrapper is an inline function in aie/<tileBase>/src/<tileBase>.cc
// (definition line carries __attribute__((always_inline)) or a plain inline).
// False when the source is not found or kernelWrapper is not inline.
bool
isKernelWrapperInline(const std::string& tileBase);

// Parse aie/<tileBase>/Release/<tileBase>.lst for the inlined-kernelWrapper inner
// loop: isolate main, find the single indirect 'jl pN' dispatch, then the first
// backward branch after it. Returns {start_pc, stop_pc} = {branch target (loop
// header), branch instruction address}, or std::nullopt if not found / invalid.
std::optional<std::pair<uint32_t, uint32_t>>
getStaticStartStopPcFromLst(const std::string& tileBase);

} // namespace xdp

#endif
