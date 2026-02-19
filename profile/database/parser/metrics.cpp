// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved

#include "metrics.h"
#include "parser_utils.h"

namespace xdp {

    void 
    Metric::print() const {
        std::cout << "Metric: " << metric;
        if (channels.has_value()) {
            std::cout << ", Channels: ";
            for (const auto& channel : *channels) {
                std::cout << static_cast<int>(channel) << " ";
            }
        }
        std::cout << std::endl;
    }

    bool
    Metric::areChannelsSet() const {
        return channels.has_value() && !channels->empty();
    }

    bool
    Metric::isChannel0Set() const {
        return channels.has_value() && !channels->empty();
    }

    bool
    Metric::isChannel1Set() const {
        return channels.has_value() && channels->size() > 1;
    }

    uint8_t
    Metric::getChannel0() const {
        if (channels.has_value() && !channels->empty()) {
            return (*channels)[0]; // Return the first channel (channel0)
        }
        return 0; // Return 0 if channels are not set or empty
    }

    uint8_t
    Metric::getChannel1() const {
        if (channels.has_value() && channels->size() > 1) {
            return (*channels)[1]; // Return the second channel (channel1)
        }
        return 1; // Return 1 if channels are not set or there is no second channel
    }

    std::optional<uint8_t>
    Metric::getChannel0Safe() const {
        if (channels.has_value() && !channels->empty()) {
            return (*channels)[0];
        }
        return std::nullopt; // Return nullopt if channels are not set or empty
    }

    std::optional<uint8_t>
    Metric::getChannel1Safe() const {
        if (channels.has_value() && channels->size() > 1) {
            return (*channels)[1];
        }
        return std::nullopt; // Return nullopt if channels are not set or there is no second channel
    }

    std::string
    Metric::getBytesToTransfer() const {
        if (bytes_to_transfer.has_value()) {
            return *bytes_to_transfer;
        }
        return ""; // or throw an exception
    }

    // Metric::Metric(std::string metric, std::optional<std::vector<uint8_t>> channels, 
    //                std::optional<std::string> bytes)
    //     : metric(std::move(metric)), channels(std::move(channels)), bytes_to_transfer(std::move(bytes)) {}

    void
    Metric::addCommonFields(boost::property_tree::ptree& obj) const {
        obj.put("metric", metric);
        if (channels.has_value()) {
            boost::property_tree::ptree channelsNode;
            for (const auto& channel : *channels) {
                boost::property_tree::ptree channelNode;
                channelNode.put("", static_cast<int>(channel)); // Convert uint8_t to int for JSON
                channelsNode.push_back(std::make_pair("", channelNode));
            }
            obj.add_child("channels", channelsNode);
        }
        if (bytes_to_transfer) {
            obj.put("bytes", *bytes_to_transfer);
        }
    }

    // --------------------------------------------------------------------------------------------------------------------
    // GraphBasedMetricEntry class Definitions
    // boost::property_tree::ptree
    // GraphBasedMetricEntry::toPtree() const {
    //     boost::property_tree::ptree obj;
    //     obj.put("graph", graph);
    //     obj.put("entity", entity);
    //     addCommonFields(obj);
    //     return obj;
    // }

    std::unique_ptr<Metric>
    GraphBasedMetricEntry::processSettings(const MetricType& type, const boost::property_tree::ptree& obj) {
        std::optional<std::vector<uint8_t>> channels = std::nullopt;
        if (obj.get_child_optional("channels")) {
            std::vector<uint8_t> parsedChannels;
            for (const auto& channelNode : obj.get_child("channels")) {
                parsedChannels.push_back(static_cast<uint8_t>(channelNode.second.get_value<int>()));
            }
            channels = parsedChannels;
        }

        // return std::make_unique<GraphBasedMetricEntry>(
        //     obj.get<std::string>("graph", "all"),
        //     obj.get<std::string>("entity", "all"),
        //     obj.get<std::string>("metric", ""),
        //     channels,
        //     obj.get_optional<std::string>("bytes") ? std::make_optional(obj.get<std::string>("bytes")) : std::nullopt
        // );

        std::string graph = obj.get<std::string>("graph", "all");
        std::string metric = obj.get<std::string>("metric", "");
        auto bytes = obj.get_optional<std::string>("bytes") ? std::make_optional(obj.get<std::string>("bytes")) : std::nullopt;
    
        if ((type == MetricType::GRAPH_BASED_AIE_TILE) || (type == MetricType::GRAPH_BASED_CORE_MOD) || (type == MetricType::GRAPH_BASED_MEM_MOD)) {
            std::string kernel = obj.get<std::string>("kernel", "all");
            return std::make_unique<AIEGraphBasedMetricEntry>(graph, kernel, metric, channels, bytes);
        }
        else if (type == MetricType::GRAPH_BASED_MEM_TILE) {
            std::string buffer = obj.get<std::string>("buffer", "all");
            return std::make_unique<MemoryTileGraphBasedMetricEntry>(graph, buffer, metric, channels, bytes);
        }
        else if (type == MetricType::GRAPH_BASED_INTERFACE_TILE) {
            std::string port = obj.get<std::string>("port", "all");
            return std::make_unique<InterfaceTileGraphBasedMetricEntry>(graph, port, metric, channels, bytes);
        }
        
        throw std::invalid_argument("Unknown module type: " + std::to_string(static_cast<int>(type)));
    }

    // void
    // GraphBasedMetricEntry::print() const {
    //     std::cout << "^^^ print GraphBasedMetricEntry- Graph:" << graph << ", Entity: " << entity;
    //     std::cout <<  ", Metric: " << metric;
    //     // std::cout << ", Channels: ";
    //     // if (channels.has_value()) {
    //     //     for (const auto& channel : *channels) {
    //     //         std::cout << static_cast<int>(channel) << " "; // Print as int for readability
    //     //     }
    //     // } else {
    //     //     std::cout << "Channels: None";
    //     // }
    //     Metric::print(); // Call the base class print method to show common fields
    // }

    // --------------------------------------------------------------------------------------------------------------------
    // TileBasedMetricEntry class Definitions
    boost::property_tree::ptree
    TileBasedMetricEntry::toPtree() const {
        boost::property_tree::ptree obj;

        // Add startTile array
        boost::property_tree::ptree startTileNode;
        for (const auto& tile : startTile) {
            boost::property_tree::ptree tileNode;
            tileNode.put("", static_cast<int>(tile)); // Convert uint8_t to int for JSON
            startTileNode.push_back(std::make_pair("", tileNode));
        }
        obj.add_child("start", startTileNode);

        // Add endTile array
        boost::property_tree::ptree endTileNode;
        for (const auto& tile : endTile) {
            boost::property_tree::ptree tileNode;
            tileNode.put("", static_cast<int>(tile)); // Convert uint8_t to int for JSON
            endTileNode.push_back(std::make_pair("", tileNode));
        }
        obj.add_child("end", endTileNode);

        addCommonFields(obj);
        return obj;
    }

    std::unique_ptr<Metric>
    TileBasedMetricEntry::processSettings(const MetricType& /*type*/, const boost::property_tree::ptree& obj) {
        std::optional<std::vector<uint8_t>> channels = std::nullopt;
        if (obj.get_child_optional("channels")) {
            std::vector<uint8_t> parsedChannels;
            for (const auto& channelNode : obj.get_child("channels")) {
                parsedChannels.push_back(static_cast<uint8_t>(channelNode.second.get_value<int>()));
            }
            channels = parsedChannels;
        }

        if (obj.get_child_optional("start") == boost::none) {
            return std::make_unique<TileBasedMetricEntry>(
                obj.get<uint8_t>("col", 0),
                obj.get<uint8_t>("row", 0),
                obj.get<std::string>("metric", "NA"),
                channels,
                obj.get_optional<std::string>("bytes") ? std::make_optional(obj.get<std::string>("bytes")) : std::nullopt
            );
        } else {
            return std::make_unique<TileBasedMetricEntry>(
                obj.get_child_optional("start") ? parseArray(obj.get_child("start")) : std::vector<uint8_t>{},
                obj.get_child_optional("end") ? parseArray(obj.get_child("end")) : std::vector<uint8_t>{},
                obj.get<std::string>("metric", "NA"),
                channels,
                obj.get_optional<std::string>("bytes") ? std::make_optional(obj.get<std::string>("bytes")) : std::nullopt
            );
        }
    }

    void
    TileBasedMetricEntry::print() const {
        std::cout << "^^^ print TileBasedMetricEntry: Start Tiles: ";
        std::cout << "Col: " << static_cast<int>(col) << ", Row: " << static_cast<int>(row) << ", Metric: " << metric;
        std::cout << ", Channels: ";
        if (channels.has_value()) {
            for (const auto& channel : *channels) {
                std::cout << static_cast<int>(channel) << " "; // Print as int for readability
            }
        } else {
            std::cout << "Channels: None";
        }
        for (const auto& tile : startTile) {
            std::cout << static_cast<int>(tile) << " "; // Print as int for readability
        }
        std::cout << ", End Tiles: ";
        for (const auto& tile : endTile) {
            std::cout << static_cast<int>(tile) << " "; // Print as int for readability
        }
        Metric::print(); // Call the base class print method to show common fields
    }
};
