// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved
#ifndef XDP_AIE_DTRACE_VE2_ELF_HELPER_H
#define XDP_AIE_DTRACE_VE2_ELF_HELPER_H

#include <vector>

namespace xdp {

// Serialize an ELF to a byte buffer from an opaque elf_impl pointer.
// Isolated in its own translation unit to avoid ELFIO / system <elf.h>
// header conflicts.
std::vector<char>
get_elf_buffer(void* elf_handle);

} // namespace xdp

#endif
