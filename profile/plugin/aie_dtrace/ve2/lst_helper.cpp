// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/ve2/lst_helper.h"

#include "core/common/message.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace xdp {

namespace {

  namespace fs = std::filesystem;
  using severity_level = xrt_core::message::severity_level;

  // End of 16KB program memory; PC_Address is bits [13:0].
  constexpr uint32_t PROG_MEM_END = 0x3FFF;

  bool endsWith(const std::string& s, const std::string& suffix)
  {
    return s.size() >= suffix.size()
        && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  std::string trimLeft(const std::string& s)
  {
    size_t i = s.find_first_not_of(" \t");
    return (i == std::string::npos) ? std::string() : s.substr(i);
  }

  // Recursively search the current working directory for the first regular file
  // whose path ends with 'suffix' (using forward-slash form). Empty on failure.
  fs::path findBySuffix(const std::string& suffix)
  {
    std::error_code ec;
    fs::path root = fs::current_path(ec);
    if (ec)
      return {};

    fs::recursive_directory_iterator it(root,
        fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      std::error_code fec;
      if (!it->is_regular_file(fec))
        continue;
      if (endsWith(it->path().generic_string(), suffix))
        return it->path();
    }
    return {};
  }

  // One disassembly instruction (or VLIW bundle) line: leading address + the
  // per-op mnemonic list (bytes stripped).
  struct InstrLine {
    uint32_t addr = 0;
    std::vector<std::string> ops;  // trimmed op strings split on ';'
  };

  // Parse a disassembly line "<addr>: <byte bytes...> \t<op1>;\t<op2>..." into its
  // leading address and the list of trimmed op strings (VLIW bundle split on ';').
  // The assembly portion starts at the first tab (after the byte columns).
  bool parseInstrLine(const std::string& line, InstrLine& out)
  {
    size_t colon = line.find(':');
    if (colon == std::string::npos)
      return false;
    // Address is the leading whitespace-prefixed hex token before ':'.
    std::string addrTok = trimLeft(line.substr(0, colon));
    if (addrTok.empty())
      return false;
    for (char c : addrTok) {
      if (!std::isxdigit(static_cast<unsigned char>(c)))
        return false;
    }
    size_t tab = line.find('\t', colon);
    if (tab == std::string::npos)
      return false;  // no assembly (e.g. "...:" continuation lines)

    try {
      out.addr = static_cast<uint32_t>(std::stoul(addrTok, nullptr, 16));
    }
    catch (...) {
      return false;
    }

    out.ops.clear();
    std::string asmText = line.substr(tab + 1);
    std::stringstream ss(asmText);
    std::string op;
    while (std::getline(ss, op, ';')) {
      std::string t = trimLeft(op);
      if (!t.empty())
        out.ops.push_back(t);
    }
    return !out.ops.empty();
  }

  // Mnemonic = first whitespace/tab-delimited token of an op.
  std::string mnemonic(const std::string& op)
  {
    size_t i = op.find_first_of(" \t");
    return (i == std::string::npos) ? op : op.substr(0, i);
  }

  // Extract "#0x<hex>" immediate target from an op; std::nullopt if none.
  std::optional<uint32_t> immTarget(const std::string& op)
  {
    size_t h = op.find("#0x");
    if (h == std::string::npos)
      return std::nullopt;
    size_t start = h + 3;
    size_t i = start;
    while (i < op.size() && std::isxdigit(static_cast<unsigned char>(op[i])))
      ++i;
    if (i == start)
      return std::nullopt;
    try {
      return static_cast<uint32_t>(std::stoul(op.substr(start, i - start), nullptr, 16));
    }
    catch (...) {
      return std::nullopt;
    }
  }

  bool opIsIndirectJl(const std::string& op)
  {
    if (mnemonic(op) != "jl")
      return false;
    // Operand is a register (e.g. "p1"); direct calls use "#0x...".
    std::string rest = trimLeft(op.substr(2));
    return !rest.empty() && rest[0] == 'p';
  }

  bool opIsBranchWithTarget(const std::string& op, uint32_t& target)
  {
    std::string m = mnemonic(op);
    if (m != "j" && m != "jz" && m != "jnz")
      return false;
    auto t = immTarget(op);
    if (!t)
      return false;
    target = *t;
    return true;
  }

} // namespace

bool
isKernelWrapperInline(const std::string& tileBase)
{
  const std::string suffix = "aie/" + tileBase + "/src/" + tileBase + ".cc";
  fs::path srcPath = findBySuffix(suffix);
  if (srcPath.empty()) {
    xrt_core::message::send(severity_level::debug, "XRT",
        "AIE dtrace: source '" + suffix + "' not found under run dir; cannot confirm kernelWrapper inline.");
    return false;
  }

  std::ifstream f(srcPath);
  if (!f.is_open())
    return false;

  // Scan for a kernelWrapper definition marked inline. We look at "void kernelWrapper("
  // lines only (ignores call sites like "kernelWrapper(args, ...)"); a bare/extern
  // declaration without the inline attribute is skipped so it does not mask a later
  // inline definition.
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("void kernelWrapper(") != std::string::npos
        && (line.find("always_inline") != std::string::npos
            || line.find("inline") != std::string::npos)) {
      xrt_core::message::send(severity_level::debug, "XRT",
          "AIE dtrace: kernelWrapper is inline in " + srcPath.generic_string());
      return true;
    }
  }
  return false;
}

std::optional<std::pair<uint32_t, uint32_t>>
getStaticStartStopPcFromLst(const std::string& tileBase)
{
  const std::string suffix = "aie/" + tileBase + "/Release/" + tileBase + ".lst";
  fs::path lstPath = findBySuffix(suffix);
  if (lstPath.empty()) {
    xrt_core::message::send(severity_level::warning, "XRT",
        "AIE dtrace: listing '" + suffix + "' not found under run dir; skipping compute_io_bound.");
    return std::nullopt;
  }

  std::ifstream f(lstPath);
  if (!f.is_open())
    return std::nullopt;

  // Collect function labels (addr, name) in file (address) order and all
  // instruction lines (addr, ops).
  std::vector<std::pair<uint32_t, std::string>> labels;
  std::vector<InstrLine> instrs;

  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    // Label line: "<8-hex-digit addr> <name>:" (a function symbol or a compiler-local ".L*" label).
    if (line.size() > 10 && std::isxdigit(static_cast<unsigned char>(line[0]))
        && line.find(" <") != std::string::npos && endsWith(line, ">:")) {
      size_t lt = line.find(" <");
      std::string addrTok = line.substr(0, lt);
      std::string name = line.substr(lt + 2, line.size() - (lt + 2) - 2);  // between "<" and ">:"
      bool hexAddr = addrTok.size() == 8
          && std::all_of(addrTok.begin(), addrTok.end(),
                         [](char c){ return std::isxdigit(static_cast<unsigned char>(c)); });
      if (hexAddr) {
        try {
          labels.emplace_back(static_cast<uint32_t>(std::stoul(addrTok, nullptr, 16)), name);
        }
        catch (...) {}
        continue;
      }
    }

    InstrLine il;
    if (parseInstrLine(line, il))
      instrs.push_back(std::move(il));
  }

  // Find main and the next FUNCTION label after it (main region = [mainAddr, mainEnd)).
  // Skip compiler-local basic-block labels (names starting with ".") which live inside a function.
  uint32_t mainAddr = 0;
  uint32_t mainEnd = PROG_MEM_END;
  bool foundMain = false;
  for (size_t i = 0; i < labels.size(); ++i) {
    if (labels[i].second == "main") {
      mainAddr = labels[i].first;
      foundMain = true;
      for (size_t j = i + 1; j < labels.size(); ++j) {
        if (!labels[j].second.empty() && labels[j].second[0] != '.') {
          mainEnd = labels[j].first;
          break;
        }
      }
      break;
    }
  }
  if (!foundMain) {
    xrt_core::message::send(severity_level::warning, "XRT",
        "AIE dtrace: 'main' not found in " + lstPath.generic_string() + "; skipping compute_io_bound.");
    return std::nullopt;
  }

  // Within main: require exactly one indirect 'jl pN' dispatch.
  int indirectJlCount = 0;
  uint32_t jlAddr = 0;
  for (const auto& il : instrs) {
    if (il.addr < mainAddr || il.addr >= mainEnd)
      continue;
    for (const auto& op : il.ops) {
      if (opIsIndirectJl(op)) {
        ++indirectJlCount;
        jlAddr = il.addr;
      }
    }
  }
  if (indirectJlCount != 1) {
    xrt_core::message::send(severity_level::warning, "XRT",
        "AIE dtrace: expected exactly one indirect 'jl pN' in main, found "
        + std::to_string(indirectJlCount) + " in " + lstPath.generic_string()
        + "; skipping compute_io_bound.");
    return std::nullopt;
  }

  // First backward branch after the indirect jl: target = start_pc, addr = stop_pc.
  for (const auto& il : instrs) {
    if (il.addr <= jlAddr || il.addr >= mainEnd)
      continue;
    for (const auto& op : il.ops) {
      uint32_t target = 0;
      if (opIsBranchWithTarget(op, target) && target < il.addr) {
        uint32_t startPc = target;
        uint32_t stopPc = il.addr;
        if (startPc < stopPc && stopPc <= PROG_MEM_END) {
          std::stringstream msg;
          msg << "AIE dtrace: compute_io_bound start/stop PCs from " << lstPath.generic_string()
              << ": start_pc=0x" << std::hex << startPc << " stop_pc=0x" << stopPc;
          xrt_core::message::send(severity_level::info, "XRT", msg.str());
          return std::make_pair(startPc, stopPc);
        }
        xrt_core::message::send(severity_level::warning, "XRT",
            "AIE dtrace: invalid start/stop PC range parsed; skipping compute_io_bound.");
        return std::nullopt;
      }
    }
  }

  xrt_core::message::send(severity_level::warning, "XRT",
      "AIE dtrace: no backward branch found after kernel dispatch in "
      + lstPath.generic_string() + "; skipping compute_io_bound.");
  return std::nullopt;
}

} // namespace xdp
