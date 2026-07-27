// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2022-2025 Advanced Micro Devices, Inc. All rights reserved

#ifndef BASE_FILETYPE_DOT_H
#define BASE_FILETYPE_DOT_H

#include <boost/property_tree/ptree.hpp>
#include <cstdint>
#include <optional>
#include "xdp/profile/database/static_info/aie_constructs.h"

namespace xdp::aie {
class BaseFiletypeImpl {
    protected:
        boost::property_tree::ptree aie_meta;

    public:
        BaseFiletypeImpl(boost::property_tree::ptree& aie_project) : aie_meta(aie_project) {}
        BaseFiletypeImpl() = delete; 
        virtual ~BaseFiletypeImpl() {};

        // Returns the wrapper PC (reloadable ELF entry PC) for the core tile at
        // column 0, row 0 from the top-level "elfs_metadata" section. All
        // "reloadable_elfs" values in that entry must be identical; otherwise
        // (or if the entry is missing/empty) std::nullopt is returned and the
        // caller must skip configuration.
        std::optional<uint32_t>
        getReloadableElfEntryPC() const
        {
            auto elfsMetadata = aie_meta.get_child_optional("elfs_metadata");
            if (!elfsMetadata)
                return std::nullopt;

            for (const auto& entry : elfsMetadata.get()) {
                auto column = entry.second.get_optional<int>("column");
                auto row = entry.second.get_optional<int>("row");
                if (!column || !row || *column != 0 || *row != 0)
                    continue;

                auto reloadableElfs = entry.second.get_child_optional("reloadable_elfs");
                if (!reloadableElfs)
                    return std::nullopt;

                std::optional<uint32_t> commonPC;
                for (const auto& elf : reloadableElfs.get()) {
                    auto pc = static_cast<uint32_t>(elf.second.get_value<uint64_t>());
                    if (!commonPC)
                        commonPC = pc;
                    else if (*commonPC != pc)
                        return std::nullopt;
                }
                return commonPC;
            }

            return std::nullopt;
        }

        // Returns the static ELF tile base name (e.g. "0_0") for the core tile at
        // column 0, row 0 from "elfs_metadata" (the first key under "static_elfs").
        // This is the RELATIVE-row listing base used to locate 0_0.lst / 0_0.cc.
        // std::nullopt if the entry or its static_elfs are missing.
        std::optional<std::string>
        getStaticElfTileName() const
        {
            auto elfsMetadata = aie_meta.get_child_optional("elfs_metadata");
            if (!elfsMetadata)
                return std::nullopt;

            for (const auto& entry : elfsMetadata.get()) {
                auto column = entry.second.get_optional<int>("column");
                auto row = entry.second.get_optional<int>("row");
                if (!column || !row || *column != 0 || *row != 0)
                    continue;

                auto staticElfs = entry.second.get_child_optional("static_elfs");
                if (!staticElfs)
                    return std::nullopt;
                for (const auto& e : staticElfs.get())
                    return e.first;  // first (and typically only) key, e.g. "0_0"
                return std::nullopt;
            }

            return std::nullopt;
        }

        // Top level interface used for both file type formats
        
        virtual driver_config
        getDriverConfig() const = 0;
        
        virtual int getHardwareGeneration() const = 0;
        virtual double getAIEClockFreqMHz() const = 0;
        
        virtual aiecompiler_options
        getAIECompilerOptions() const = 0;
        
        virtual uint8_t getNumRows() const = 0;

        virtual uint8_t getAIETileRowOffset() const = 0;

        virtual std::vector<uint8_t>
        getPartitionOverlayStartCols() const = 0;

        virtual std::vector<std::string>
        getValidGraphs() const = 0;

        virtual std::vector<std::string>
        getValidPorts() const = 0;

        virtual std::vector<std::string>
        getValidKernels() const = 0;

        virtual std::vector<std::string>
        getValidBuffers() const = 0;

        virtual std::unordered_map<std::string, io_config>
        getTraceGMIOs() const = 0;

        virtual std::unordered_map<std::string, io_config>
        getGMIOs() const = 0;

        virtual std::vector<tile_type>
        getMicrocontrollers(bool useColumn = false,
                            uint8_t minCol = 0,
                            uint8_t maxCol = 0) const = 0;

        virtual 
        std::vector<tile_type>
        getInterfaceTiles(const std::string& graphName,
                          const std::string& portName = "all",
                          const std::string& metricStr = "channels",
                          int16_t specifiedId = -1,
                          bool useColumn = false, 
                          uint8_t minCol = 0, 
                          uint8_t maxCol = 0) const = 0;

        virtual 
        std::vector<tile_type>
        getMemoryTiles(const std::string& graphName,
                       const std::string& bufferName) const = 0;

        virtual std::vector<tile_type>
        getAIETiles(const std::string& graphName) const = 0;

        virtual std::vector<tile_type>
        getAllAIETiles(const std::string& graphName) const = 0;

        virtual std::vector<tile_type>
        getEventTiles(const std::string& graph_name,
                      module_type type) const = 0;

        virtual std::vector<tile_type>
        getTiles(const std::string& graph_name,
                 module_type type, 
                 const std::string& kernel_name) const = 0;

        virtual std::vector<UCInfo>
        getActiveMicroControllers() const = 0;
};

}


#endif