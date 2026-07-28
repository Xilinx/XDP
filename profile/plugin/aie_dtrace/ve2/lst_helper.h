// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved
#ifndef XDP_AIE_DTRACE_VE2_LST_HELPER_H
#define XDP_AIE_DTRACE_VE2_LST_HELPER_H

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace xdp {

// Derive the compute_io_bound compute-window start/stop PCs from the aiecompiler
// listing (.lst) of a core tile. Used for BOTH design types:
//   - reloadable: listings <tileBase>_reloadable*.lst (kernelWrapper is a real
//     function); every listing must agree on the PCs.
//   - static:     listing <tileBase>.lst (kernelWrapper is inlined into main).
//
// start_pc = PC of the single indirect kernel dispatch ("jl pN"; direct calls
//            use "jl #0x...").
// stop_pc  = the 10th instruction listed from start_pc (the jl counts as #1),
//            clamped to the last instruction of the enclosing label.
//
// tileBase is the RELATIVE-row listing base name of the first core tile ("0_0").
// Listings are located by a depth-bounded search for the design's "aie" directory,
// starting at XRT_AIE_DTRACE_DESIGN_DIR when set, otherwise the current working
// directory (the run dir, where the design's aiecompiler Work tree is co-located).
// The result is cached, so the search runs at most once per tile per process.
//
// Returns {start_pc, stop_pc}, or std::nullopt if the listing is missing or the
// expected dispatch/instruction pattern is not found.
std::optional<std::pair<uint32_t, uint32_t>>
getComputeStartStopPc(const std::string& tileBase);

} // namespace xdp

#endif
