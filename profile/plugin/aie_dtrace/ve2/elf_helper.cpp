// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved

// IMPORTANT: elf_int.h (which includes ELFIO) must be included before any
// system headers that pull in <elf.h>, to avoid macro/constexpr conflicts
// between system ELF relocation macros and ELFIO's constexpr definitions.
#define XDP_PLUGIN_SOURCE

#include "core/common/api/elf_int.h"

#include "xdp/profile/plugin/aie_dtrace/ve2/elf_helper.h"

#include <sstream>

namespace xdp {

std::vector<char>
get_elf_buffer(void* elf_handle)
{
  auto* impl = static_cast<xrt::elf_impl*>(elf_handle);
  auto& elfio_ref = impl->get_elfio();

  std::stringstream ss;
  ss << std::noskipws;
  const_cast<ELFIO::elfio&>(elfio_ref).save(ss);

  auto str = ss.str();
  return {str.begin(), str.end()};
}

} // namespace xdp
