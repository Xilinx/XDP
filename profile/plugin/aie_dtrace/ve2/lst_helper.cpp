// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved

#define XDP_PLUGIN_SOURCE

#include "xdp/profile/plugin/aie_dtrace/ve2/lst_helper.h"

#include "core/common/message.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace xdp {

namespace {

  namespace fs = std::filesystem;
  using severity_level = xrt_core::message::severity_level;

  // End of 16KB program memory; PC_Address is bits [13:0].
  constexpr uint32_t PROG_MEM_END = 0x3FFF;

  // stop_pc is this many listed instructions from the dispatch, counting the
  // dispatch itself as #1.
  constexpr int STOP_PC_INSTR_COUNT = 10;

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

  // Depth limit for the design-directory search, relative to the search root. A
  // design listing sits ~8 levels down (<design>/vaiml_par_*/<n>/aiecompiler/Work/
  // aie/<tile>/Release/<tile>.lst), so this is generous while bounding the walk.
  constexpr int MAX_SEARCH_DEPTH = 12;

  // Root for the listing search: the XRT_AIE_DTRACE_DESIGN_DIR override when set,
  // otherwise the current working directory (the run dir).
  fs::path searchRoot()
  {
    if (const char* env = std::getenv("XRT_AIE_DTRACE_DESIGN_DIR")) {
      if (env[0] != '\0') {
        std::error_code eec;
        fs::path p(env);
        if (fs::is_directory(p, eec))
          return p;
        xrt_core::message::send(severity_level::warning, "XRT",
            std::string("AIE dtrace: XRT_AIE_DTRACE_DESIGN_DIR='") + env
            + "' is not a directory; falling back to the run directory.");
      }
    }
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    return ec ? fs::path{} : cwd;
  }

  // Search only for the two listing filenames we need:
  //   reloadable design: "<tileBase>_reloadable*.lst"
  //   static design:     "<tileBase>.lst"
  // Reloadable listings win when both are present (in a reloadable design the tile's
  // own <tileBase>.lst holds no kernel dispatch).
  //
  // The walk is kept cheap by pruning: it is depth-bounded, skips hidden directories,
  // and inside a design "aie" directory it only descends into the tile directories of
  // interest (<tileBase> and <tileBase>_reloadable*), never the hundreds of other
  // per-tile directories. Only the first file per unique filename is kept, so a run
  // directory holding several copies of the same design Work tree yields no duplicates.
  std::vector<fs::path> collectTileListings(const std::string& tileBase, bool& reloadable)
  {
    const std::string staticName = tileBase + ".lst";
    const std::string reloadablePrefix = tileBase + "_reloadable";

    const fs::path root = searchRoot();
    if (root.empty())
      return {};

    std::vector<fs::path> reloadableLst;
    std::vector<fs::path> staticLst;
    std::vector<std::string> seen;

    std::error_code ec;
    fs::recursive_directory_iterator it(root,
        fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      const std::string name = it->path().filename().string();

      std::error_code dec;
      if (it->is_directory(dec)) {
        if (it.depth() >= MAX_SEARCH_DEPTH || (!name.empty() && name[0] == '.')) {
          it.disable_recursion_pending();
          continue;
        }
        // Within a design "aie" directory, only the tile(s) we care about are worth
        // descending into.
        if (it->path().parent_path().filename() == "aie"
            && name != tileBase
            && name.compare(0, reloadablePrefix.size(), reloadablePrefix) != 0)
          it.disable_recursion_pending();
        continue;
      }

      if (!endsWith(name, ".lst"))
        continue;
      const bool isReloadable = name.compare(0, reloadablePrefix.size(), reloadablePrefix) == 0;
      if (!isReloadable && name != staticName)
        continue;
      if (std::find(seen.begin(), seen.end(), name) != seen.end())
        continue;
      seen.push_back(name);
      (isReloadable ? reloadableLst : staticLst).push_back(it->path());
    }

    if (!reloadableLst.empty()) {
      reloadable = true;
      std::sort(reloadableLst.begin(), reloadableLst.end());
      return reloadableLst;
    }
    reloadable = false;
    std::sort(staticLst.begin(), staticLst.end());
    return staticLst;
  }

  // One entry of the listing: either a label or an instruction line.
  struct ListingEntry {
    uint32_t addr = 0;
    bool isLabel = false;
    bool isIndirectDispatch = false;  // instruction is "jl pN"
  };

  // Mnemonic = first whitespace/tab-delimited token of an op.
  std::string mnemonic(const std::string& op)
  {
    size_t i = op.find_first_of(" \t");
    return (i == std::string::npos) ? op : op.substr(0, i);
  }

  // "jl pN" is the indirect kernel dispatch; "jl #0x..." is a direct call.
  bool opIsIndirectJl(const std::string& op)
  {
    if (mnemonic(op) != "jl")
      return false;
    std::string rest = trimLeft(op.substr(2));
    return !rest.empty() && rest[0] == 'p';
  }

  // Parse a label line "<8-hex-digit addr> <name>:".
  bool parseLabelLine(const std::string& line, uint32_t& addr)
  {
    if (line.size() <= 10 || !std::isxdigit(static_cast<unsigned char>(line[0])))
      return false;
    size_t lt = line.find(" <");
    if (lt == std::string::npos || !endsWith(line, ">:"))
      return false;
    std::string addrTok = line.substr(0, lt);
    if (addrTok.size() != 8
        || !std::all_of(addrTok.begin(), addrTok.end(),
                        [](char c){ return std::isxdigit(static_cast<unsigned char>(c)); }))
      return false;
    try {
      addr = static_cast<uint32_t>(std::stoul(addrTok, nullptr, 16));
    }
    catch (...) {
      return false;
    }
    return true;
  }

  // Parse a disassembly line "<addr>: <bytes...> \t<op1>;\t<op2>..." into its
  // leading address, and report whether any op is the indirect dispatch.
  // The assembly portion starts at the first tab (after the byte columns).
  bool parseInstrLine(const std::string& line, uint32_t& addr, bool& indirectDispatch)
  {
    size_t colon = line.find(':');
    if (colon == std::string::npos)
      return false;
    std::string addrTok = trimLeft(line.substr(0, colon));
    if (addrTok.empty()
        || !std::all_of(addrTok.begin(), addrTok.end(),
                        [](char c){ return std::isxdigit(static_cast<unsigned char>(c)); }))
      return false;
    size_t tab = line.find('\t', colon);
    if (tab == std::string::npos)
      return false;  // no assembly text (e.g. an elision "..." line)

    try {
      addr = static_cast<uint32_t>(std::stoul(addrTok, nullptr, 16));
    }
    catch (...) {
      return false;
    }

    indirectDispatch = false;
    std::stringstream ss(line.substr(tab + 1));
    std::string op;
    while (std::getline(ss, op, ';')) {
      std::string t = trimLeft(op);
      if (!t.empty() && opIsIndirectJl(t))
        indirectDispatch = true;
    }
    return true;
  }

  // Read a listing into ordered labels/instructions. "..." elision lines carry no
  // address and are skipped, so they are neither counted nor expanded.
  std::vector<ListingEntry> readListing(const fs::path& path)
  {
    std::vector<ListingEntry> entries;
    std::ifstream f(path);
    if (!f.is_open())
      return entries;

    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();

      ListingEntry e;
      if (parseLabelLine(line, e.addr)) {
        e.isLabel = true;
        entries.push_back(e);
        continue;
      }
      if (parseInstrLine(line, e.addr, e.isIndirectDispatch)) {
        e.isLabel = false;
        entries.push_back(e);
      }
    }
    return entries;
  }

  // start_pc = the single indirect dispatch; stop_pc = the STOP_PC_INSTR_COUNT'th
  // listed instruction from it (dispatch = #1), clamped to the last instruction
  // before the next label.
  std::optional<std::pair<uint32_t, uint32_t>>
  startStopFromListing(const fs::path& path)
  {
    const std::vector<ListingEntry> entries = readListing(path);

    int dispatchCount = 0;
    size_t dispatchIdx = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
      if (!entries[i].isLabel && entries[i].isIndirectDispatch) {
        ++dispatchCount;
        dispatchIdx = i;
      }
    }
    if (dispatchCount != 1) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "AIE dtrace: expected exactly one indirect 'jl pN' dispatch, found "
          + std::to_string(dispatchCount) + " in " + path.generic_string()
          + "; skipping compute_io_bound.");
      return std::nullopt;
    }

    const uint32_t startPc = entries[dispatchIdx].addr;
    uint32_t stopPc = startPc;
    int counted = 1;  // the dispatch itself is instruction #1
    for (size_t i = dispatchIdx + 1; i < entries.size() && counted < STOP_PC_INSTR_COUNT; ++i) {
      if (entries[i].isLabel) {
        // End of the enclosing label: stop here and use the last instruction seen.
        xrt_core::message::send(severity_level::debug, "XRT",
            "AIE dtrace: label boundary reached after " + std::to_string(counted)
            + " instructions; clamping stop_pc in " + path.generic_string());
        break;
      }
      stopPc = entries[i].addr;
      ++counted;
    }

    if (startPc >= stopPc || stopPc > PROG_MEM_END) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "AIE dtrace: invalid start/stop PCs from " + path.generic_string()
          + "; skipping compute_io_bound.");
      return std::nullopt;
    }
    return std::make_pair(startPc, stopPc);
  }

  // Locate the tile listings and derive the compute start/stop PCs from them.
  std::optional<std::pair<uint32_t, uint32_t>>
  resolveComputeStartStopPc(const std::string& tileBase)
  {
    bool reloadable = false;
    const std::vector<fs::path> listings = collectTileListings(tileBase, reloadable);
    if (listings.empty()) {
      xrt_core::message::send(severity_level::warning, "XRT",
          "AIE dtrace: no listing found for tile '" + tileBase
          + "' under the design/run directory (set XRT_AIE_DTRACE_DESIGN_DIR to point at"
            " the design); skipping compute_io_bound.");
      return std::nullopt;
    }

    // All listings must agree on the PCs (trivially true for the single static one).
    std::optional<std::pair<uint32_t, uint32_t>> common;
    for (const auto& path : listings) {
      auto pcs = startStopFromListing(path);
      if (!pcs)
        return std::nullopt;
      if (!common) {
        common = pcs;
      }
      else if (*common != *pcs) {
        std::stringstream msg;
        msg << "AIE dtrace: reloadable listings disagree on kernelWrapper PCs ("
            << std::hex << "0x" << common->first << "/0x" << common->second
            << " vs 0x" << pcs->first << "/0x" << pcs->second << std::dec
            << ") in " << path.generic_string() << "; skipping compute_io_bound.";
        xrt_core::message::send(severity_level::warning, "XRT", msg.str());
        return std::nullopt;
      }
    }

    std::stringstream msg;
    msg << "AIE dtrace: compute_io_bound " << (reloadable ? "reloadable" : "static")
        << " design, start_pc=0x" << std::hex << common->first
        << " stop_pc=0x" << common->second << std::dec
        << " (from " << listings.size() << " listing(s))";
    xrt_core::message::send(severity_level::info, "XRT", msg.str());

    return common;
  }

} // namespace

std::optional<std::pair<uint32_t, uint32_t>>
getComputeStartStopPc(const std::string& tileBase)
{
  // generateCTForRun runs per kernel run, but the listings are fixed for the loaded
  // design: resolve once (the directory search is the expensive part) and reuse.
  static std::mutex mtx;
  static std::map<std::string, std::optional<std::pair<uint32_t, uint32_t>>> cache;

  std::lock_guard<std::mutex> lock(mtx);
  auto hit = cache.find(tileBase);
  if (hit != cache.end())
    return hit->second;

  auto result = resolveComputeStartStopPc(tileBase);
  cache.emplace(tileBase, result);
  return result;
}

} // namespace xdp
